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
/// Same-room is the caller's check, exactly as in matrix-sdk's own validator,
/// and so is encryption: see `encryption_permits`. A missing timestamp orders
/// first.
pub(crate) fn parse(event: &Value) -> Option<Replacement<'_>> {
    if !is_replacement(event) || event.get("state_key").is_some() {
        return None;
    }
    let content = event.get("content")?;
    Some(Replacement {
        event_id: event.get("event_id")?.as_str()?,
        sender: event.get("sender")?.as_str()?,
        ts: event
            .get("origin_server_ts")
            .and_then(Value::as_u64)
            .unwrap_or(0),
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

/// An encrypted original accepts only an encrypted replacement, so a
/// homeserver cannot rewrite it with a plaintext event.
pub(crate) fn encryption_permits(replacement_encrypted: bool, original_encrypted: bool) -> bool {
    replacement_encrypted || !original_encrypted
}

/// `m.new_content` replaces the whole content, so it must be a message on its
/// own: a body, and for an attachment the file it now shows.
pub(crate) fn complete_message(new_content: &Value) -> bool {
    if !new_content.get("body").is_some_and(Value::is_string) {
        return false;
    }
    match new_content.get("msgtype").and_then(Value::as_str) {
        Some("m.image" | "m.video" | "m.audio" | "m.file") => {
            new_content.get("file").is_some_and(Value::is_object)
                || new_content
                    .get("url")
                    .and_then(Value::as_str)
                    .is_some_and(|u| u.starts_with("mxc://"))
        }
        _ => true,
    }
}

/// The original as its winning replacement shows it: `m.new_content` whole,
/// except that the original's own `m.relates_to` (thread, reply) is kept.
pub(crate) fn project(original_content: &Value, new_content: &Value) -> Value {
    let mut out = new_content.clone();
    if let Some(fields) = out.as_object_mut() {
        fields.remove("m.relates_to");
        if let Some(relation) = original_content.get("m.relates_to") {
            fields.insert("m.relates_to".to_owned(), relation.clone());
        }
    }
    out
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
    fn an_encrypted_original_needs_an_encrypted_replacement() {
        assert!(!encryption_permits(false, true));
        assert!(encryption_permits(true, true));
        assert!(encryption_permits(false, false));
        assert!(encryption_permits(true, false));
    }

    #[test]
    fn a_projection_is_the_new_content_with_the_originals_relations() {
        let original = json!({"msgtype": "m.file", "body": "a.pdf", "url": "mxc://x/a",
                              "m.relates_to": {"rel_type": "m.thread", "event_id": "$root"}});
        let new = json!({"msgtype": "m.text", "body": "never mind",
                         "m.relates_to": {"rel_type": "m.thread", "event_id": "$other"}});
        let projected = project(&original, &new);
        assert_eq!(projected["body"], "never mind");
        assert!(projected.get("url").is_none(), "the old attachment survived the edit");
        assert_eq!(projected["m.relates_to"]["event_id"], "$root");
        assert!(complete_message(&new));
        assert!(!complete_message(&json!({"msgtype": "m.image", "body": "caption"})));
        assert!(complete_message(&json!({"msgtype": "m.image", "body": "c", "url": "mxc://x/b"})));
        assert!(!complete_message(&json!({"msgtype": "m.text"})));
    }

    #[test]
    fn the_newest_wins_and_equal_times_go_to_the_greatest_id() {
        assert!(supersedes((2, "$a"), (1, "$z")));
        assert!(!supersedes((1, "$z"), (2, "$a")));
        assert!(supersedes((1, "$b"), (1, "$a")));
        assert!(!supersedes((1, "$a"), (1, "$a")));
    }
}
