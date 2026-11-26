//! "Remove recent messages": one bounded backward walk collecting a member's
//! unredacted events. Nothing is redacted here.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::room::MessagesOptions;
use serde_json::json;

use crate::{enqueue, RustClient};

const PAGE: u32 = 100;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Kind {
    Message,
    State,
}

/// `None` for anything that is not a removable event sent by `user_id`.
pub(crate) fn classify(event: &serde_json::Value, user_id: &str) -> Option<Kind> {
    if event.get("sender").and_then(|v| v.as_str()) != Some(user_id) {
        return None;
    }
    event.get("event_id").and_then(|v| v.as_str())?;
    let kind = event.get("type").and_then(|v| v.as_str())?;
    // A redaction cannot be undone by redacting it, and removing the create
    // event would break the room for everyone.
    if kind == "m.room.redaction" || kind == "m.room.create" {
        return None;
    }
    if event
        .get("unsigned")
        .and_then(|unsigned| unsigned.get("redacted_because"))
        .is_some()
    {
        return None;
    }
    Some(if event.get("state_key").is_some() { Kind::State } else { Kind::Message })
}

#[derive(Debug, Default, PartialEq, Eq)]
pub(crate) struct Collected {
    pub(crate) messages: Vec<String>,
    pub(crate) state: Vec<String>,
}

impl Collected {
    pub(crate) fn take_page(&mut self, page: &[serde_json::Value], user_id: &str) {
        for event in page {
            let Some(kind) = classify(event, user_id) else { continue };
            let id = event["event_id"].as_str().unwrap_or_default().to_owned();
            match kind {
                Kind::Message => self.messages.push(id),
                Kind::State => self.state.push(id),
            }
        }
    }
}

/// Walks back from `from` (the live end when empty) for at most
/// `max_requests` pages of `PAGE` events. `next_from` is where a further walk
/// resumes; empty once the room's start was reached.
pub(crate) fn user_recent_events(
    bridge: &RustClient,
    room_id: String,
    user_id: String,
    from: String,
    max_requests: u32,
    op_id: u64,
) -> Result<(), String> {
    let client = crate::rooms::require_client(bridge)?;
    let room = crate::rooms::joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    let max_requests = max_requests.clamp(1, 20);
    bridge.spawn_room_action(async move {
        let mut collected = Collected::default();
        let mut token = (!from.is_empty()).then_some(from);
        let mut ok = true;
        let mut next_from = String::new();
        for request in 0..max_requests {
            let mut options = MessagesOptions::backward();
            options.limit = PAGE.into();
            options.from = token.clone();
            let page = match tokio::time::timeout(
                Duration::from_secs(20),
                room.messages(options),
            )
            .await
            {
                Ok(Ok(page)) => page,
                _ => {
                    ok = request > 0;
                    next_from = token.clone().unwrap_or_default();
                    break;
                }
            };
            let values: Vec<serde_json::Value> = page
                .chunk
                .iter()
                .filter_map(|event| serde_json::from_str(event.raw().json().get()).ok())
                .collect();
            collected.take_page(&values, &user_id);
            match page.end {
                Some(end) if !page.chunk.is_empty() => {
                    if request + 1 == max_requests {
                        next_from = end.clone();
                    }
                    token = Some(end);
                }
                _ => break,
            }
        }
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "user_recent_events",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "user_id": user_id,
                "ok": ok,
                "next_from": next_from,
                "messages": collected.messages,
                "state": collected.state,
            }),
        );
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SPAMMER: &str = "@spam:example.org";

    fn event(id: &str, sender: &str, kind: &str) -> serde_json::Value {
        json!({ "event_id": id, "sender": sender, "type": kind, "content": {} })
    }

    #[test]
    fn only_the_members_unredacted_events_count() {
        let mut state = event("$s", SPAMMER, "m.room.member");
        state["state_key"] = json!(SPAMMER);
        let mut redacted = event("$r", SPAMMER, "m.room.message");
        redacted["unsigned"] = json!({ "redacted_because": { "type": "m.room.redaction" } });
        assert_eq!(classify(&event("$m", SPAMMER, "m.room.message"), SPAMMER), Some(Kind::Message));
        assert_eq!(classify(&event("$e", SPAMMER, "m.room.encrypted"), SPAMMER), Some(Kind::Message));
        assert_eq!(classify(&event("$x", SPAMMER, "m.reaction"), SPAMMER), Some(Kind::Message));
        assert_eq!(classify(&state, SPAMMER), Some(Kind::State));
        assert_eq!(classify(&redacted, SPAMMER), None);
        assert_eq!(classify(&event("$o", "@other:example.org", "m.room.message"), SPAMMER), None);
        assert_eq!(classify(&event("$d", SPAMMER, "m.room.redaction"), SPAMMER), None);
        let mut create = event("$c", SPAMMER, "m.room.create");
        create["state_key"] = json!("");
        assert_eq!(classify(&create, SPAMMER), None);
    }

    #[test]
    fn a_page_keeps_only_the_members_events_in_order() {
        let mut page: Vec<_> = (0..4).map(|i| event(&format!("${i}"), SPAMMER, "m.room.message")).collect();
        page.insert(2, event("$o", "@other:example.org", "m.room.message"));
        let mut collected = Collected::default();
        collected.take_page(&page, SPAMMER);
        assert_eq!(collected.messages, vec!["$0", "$1", "$2", "$3"]);
        assert!(collected.state.is_empty());
    }
}
