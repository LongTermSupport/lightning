use serde_json::{json, Value};

pub(crate) const ROOM: &str = "!history:localhost";
pub(crate) const OTHER_ROOM: &str = "!other:localhost";
pub(crate) const ORIGINAL: &str = "$original";
const OWNER: &str = "@owner:localhost";

#[derive(Clone, Copy, PartialEq)]
pub(crate) enum Projection {
    Search,
    Media,
    Export,
    Preview,
}

pub(crate) struct Scenario {
    pub name: &'static str,
    pub original: Option<Value>,
    pub original_encrypted: bool,
    pub edits: Vec<Value>,
    pub before_redaction: Option<Value>,
    pub expected: Option<Value>,
}

impl Scenario {
    /// Newest first, as a backward walk meets them: edits, then the original.
    pub fn history<'a>(&'a self, room: &'a str) -> impl Iterator<Item = (&'a Value, bool)> + 'a {
        let encrypted = self.original_encrypted;
        self.edits.iter().map(|e| (e, false))
            .chain(self.original.iter().map(move |e| (e, encrypted)))
            .filter(move |(e, _)| e["room_id"] == room)
    }

    /// What the projection should show for the original; `None` when it has no row.
    pub fn content_for(&self, projection: Projection) -> Option<&Value> {
        // Scope, not divergence: search and media history never list state events, and search only messages.
        if let Some(original) = &self.original {
            if matches!(projection, Projection::Search | Projection::Media)
                && original.get("state_key").is_some()
            {
                return None;
            }
            if projection == Projection::Search && original["type"] == "m.sticker" {
                return None;
            }
        }
        self.expected.as_ref()
    }
}

fn message(id: &str, ts: u64, content: &Value) -> Value {
    json!({"type": "m.room.message", "room_id": ROOM, "sender": OWNER,
           "event_id": id, "origin_server_ts": ts, "content": content})
}

fn edit(id: &str, ts: u64, content: &Value) -> Value {
    message(id, ts, &json!({
        "msgtype": "m.text", "body": format!("* {}", content["body"].as_str().unwrap()),
        "m.relates_to": {"rel_type": "m.replace", "event_id": ORIGINAL},
        "m.new_content": content,
    }))
}

pub(crate) fn scenarios() -> Vec<Scenario> {
    let link = |body: &str| json!({"msgtype": "m.text", "body": body});
    let old = link("https://fixture.invalid/original");
    let updated = link("https://fixture.invalid/updated");
    let other = link("https://fixture.invalid/other");
    let original = message(ORIGINAL, 10, &old);
    let owner = edit("$edit", 20, &updated);
    let case = |name, edits, expected: &Value| Scenario {
        name, original: Some(original.clone()), original_encrypted: false, edits,
        before_redaction: None, expected: Some(expected.clone()),
    };

    let mut stranger = edit("$stranger", 40, &other);
    stranger["sender"] = json!("@stranger:localhost");
    let mut foreign = owner.clone();
    foreign["room_id"] = json!(OTHER_ROOM);
    let sticker = json!({"body": "fixture sticker", "url": "mxc://localhost/sticker"});
    let mut sticker_event = message(ORIGINAL, 10, &sticker);
    sticker_event["type"] = json!("m.sticker");
    let mut state = original.clone();
    state["state_key"] = json!("");
    let mut nested = edit("$nested", 30, &other);
    nested["content"]["m.relates_to"]["event_id"] = json!("$edit");
    let mut no_content = owner.clone();
    no_content["content"].as_object_mut().unwrap().remove("m.new_content");
    let mut undated = owner.clone();
    undated.as_object_mut().unwrap().remove("origin_server_ts");
    let winner = edit("$winner", 30, &other);
    let mut redacted = winner.clone();
    // As a server keeping relations through redaction (MSC3389) serves it; otherwise nothing marks it an edit.
    redacted["content"] = json!({"m.relates_to": winner["content"]["m.relates_to"]});
    redacted["unsigned"] = json!({"redacted_because": {
        "type": "m.room.redaction", "room_id": ROOM, "sender": OWNER, "event_id": "$redaction",
        "origin_server_ts": 40, "content": {"redacts": "$winner"}, "redacts": "$winner"
    }});

    let first_file = json!({"msgtype": "m.file", "body": "fixture original file",
        "url": "mxc://localhost/first", "filename": "first.pdf",
        "info": {"mimetype": "application/pdf", "size": 77, "thumbnail_url": "mxc://localhost/thumb"}});
    let next_file = json!({"msgtype": "m.image", "body": "fixture replacement image",
        "url": "mxc://localhost/second", "filename": "second.png",
        "info": {"mimetype": "image/png", "size": 33}});
    let incomplete = json!({"msgtype": "m.image", "body": "fixture missing file", "filename": "absent.png"});

    vec![
        case("valid owner edit", vec![owner.clone()], &updated),
        case("stranger edit", vec![stranger.clone()], &old),
        case("stranger cannot hide owner's edit", vec![stranger, owner.clone()], &updated),
        case("cross-room edit", vec![foreign], &old),
        Scenario { original: Some(sticker_event),
            ..case("message edit of sticker", vec![owner.clone()], &sticker) },
        Scenario { original: Some(state),
            ..case("state-event original", vec![owner.clone()], &old) },
        case("edit of an edit", vec![nested, owner.clone()], &updated),
        case("missing m.new_content", vec![no_content], &old),
        Scenario { original_encrypted: true,
            ..case("plaintext edit of encrypted original", vec![owner.clone()], &old) },
        case("older edit arrives last", vec![winner.clone(), owner.clone()], &other),
        case("older edit arrives first", vec![owner.clone(), winner.clone()], &other),
        case("equal timestamps", vec![edit("$z", 20, &other), owner.clone()], &other),
        case("equal timestamps reversed", vec![owner.clone(), edit("$z", 20, &other)], &other),
        case("missing timestamp", vec![undated.clone()], &updated),
        case("dated edit outranks undated edit", vec![edit("$a", 1, &other), undated], &other),
        Scenario { before_redaction: Some(winner),
            ..case("redacted winning edit", vec![redacted, owner.clone()], &updated) },
        Scenario { original: None, expected: None,
            ..case("missing original", vec![owner], &old) },
        Scenario { original: Some(message(ORIGINAL, 10, &first_file)),
            ..case("attachment edit swaps the file", vec![edit("$file", 20, &next_file)], &next_file) },
        Scenario { original: Some(message(ORIGINAL, 10, &first_file)),
            ..case("incomplete attachment edit", vec![edit("$file", 20, &incomplete)], &first_file) },
    ]
}
