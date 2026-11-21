//! The spec's "Validity of replacement events" and which valid replacement
//! wins, for projections built from raw event JSON (local search, media
//! history, room export).

use serde_json::Value;

pub(crate) struct Replacement<'a> {
    pub event_id: &'a str,
    pub sender: &'a str,
    pub ts: u64,
    pub target: &'a str,
    pub new_content: &'a Value,
}

pub(crate) fn is_replacement(event: &Value) -> bool {
    event
        .pointer("/content/m.relates_to/rel_type")
        .and_then(Value::as_str)
        == Some("m.replace")
}

/// `None` when the event is not a replacement, or is one the spec says to
/// ignore whatever it targets (no `m.new_content`, a state event, malformed).
/// Same-room is the caller's check, exactly as in matrix-sdk's own validator.
pub(crate) fn parse(event: &Value) -> Option<Replacement<'_>> {
    if !is_replacement(event) || event.get("state_key").is_some() {
        return None;
    }
    let content = event.get("content")?;
    Some(Replacement {
        event_id: event.get("event_id")?.as_str()?,
        sender: event.get("sender")?.as_str()?,
        ts: event.get("origin_server_ts")?.as_u64()?,
        target: content
            .pointer("/m.relates_to/event_id")?
            .as_str()
            .filter(|id| !id.is_empty())?,
        new_content: content.get("m.new_content").filter(|c| c.is_object())?,
    })
}

/// The rules that need the original. Callers admit only non-state,
/// non-replacement events as originals.
pub(crate) fn may_replace(
    replacement_sender: &str,
    replacement_type: &str,
    original_sender: &str,
    original_type: &str,
) -> bool {
    replacement_sender == original_sender && replacement_type == original_type
}

/// The spec's order: the newest `origin_server_ts`, ties to the greatest
/// event id. Arrival order never decides.
pub(crate) fn supersedes(candidate: (u64, &str), current: (u64, &str)) -> bool {
    candidate > current
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn edit() -> Value {
        json!({"type": "m.room.message", "event_id": "$e", "sender": "@a:x",
               "origin_server_ts": 5,
               "content": {"body": "* new", "m.new_content": {"msgtype": "m.emote", "body": "new"},
                           "m.relates_to": {"rel_type": "m.replace", "event_id": "$o"}}})
    }

    #[test]
    fn a_replacement_needs_new_content_and_a_target_and_no_state_key() {
        let r = edit();
        let parsed = parse(&r).unwrap();
        assert_eq!((parsed.target, parsed.ts, parsed.event_id), ("$o", 5, "$e"));
        let mut no_new = edit();
        no_new["content"]
            .as_object_mut()
            .unwrap()
            .remove("m.new_content");
        assert!(is_replacement(&no_new) && parse(&no_new).is_none());
        let mut state = edit();
        state["state_key"] = json!("");
        assert!(parse(&state).is_none());
        let mut no_target = edit();
        no_target["content"]["m.relates_to"]["event_id"] = json!("");
        assert!(parse(&no_target).is_none());
    }

    #[test]
    fn only_the_owner_may_replace_and_only_the_same_type() {
        assert!(may_replace(
            "@a:x",
            "m.room.message",
            "@a:x",
            "m.room.message"
        ));
        assert!(!may_replace(
            "@b:x",
            "m.room.message",
            "@a:x",
            "m.room.message"
        ));
        assert!(!may_replace("@a:x", "m.room.message", "@a:x", "m.sticker"));
    }

    #[test]
    fn the_newest_wins_and_equal_times_go_to_the_greatest_id() {
        assert!(supersedes((2, "$a"), (1, "$z")));
        assert!(!supersedes((1, "$z"), (2, "$a")));
        assert!(supersedes((1, "$b"), (1, "$a")));
        assert!(!supersedes((1, "$a"), (1, "$a")));
    }
}
