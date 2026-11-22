
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};

pub(crate) const MAX_ROWS: u64 = 100_000;

pub(crate) const PAGE_SIZE: u64 = 100;

const MAX_PENDING_EDITS: usize = 8192;

/// Consecutive pages wholly older than a date range before the walk stops:
/// history order is the server's, not `origin_server_ts`, so one old event
/// proves nothing about the next page.
const DATE_GRACE_PAGES: u32 = 3;

/// Consecutive empty pages a server may hand out before the walk gives up.
const MAX_EMPTY_PAGES: u32 = 20;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum PageEnd {
    More,
    /// No `end` token: the spec's only end of history.
    End,
    /// A token seen before, or too many empty pages in a row.
    Stalled,
}

/// Backward `/messages` continuation. An empty chunk with an `end` is not the
/// end of history (Matrix allows it); a repeated token or a run of empty
/// pages is a stall, never the start of the room.
#[derive(Debug, Clone, Default)]
pub(crate) struct Pager {
    seen: HashSet<String>,
    empty_run: u32,
}

impl Pager {
    pub(crate) fn after_page(&mut self, from: Option<&str>, end: Option<&str>, events: usize) -> PageEnd {
        let Some(end) = end else { return PageEnd::End };
        if let Some(from) = from {
            self.seen.insert(from.to_owned());
        }
        if self.seen.contains(end) {
            return PageEnd::Stalled;
        }
        self.empty_run = if events == 0 { self.empty_run + 1 } else { 0 };
        if self.empty_run >= MAX_EMPTY_PAGES {
            self.empty_run = 0;
            PageEnd::Stalled
        } else {
            PageEnd::More
        }
    }

    /// A stall on a token already walked; continuing from it cannot progress.
    pub(crate) fn repeats(&self, end: Option<&str>) -> bool {
        end.is_some_and(|end| self.seen.contains(end))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Range {
    Latest(u64),
    All,
    Between { from_ms: u64, to_ms: u64 },
}

impl Range {
    pub(crate) fn from_ffi(mode: i32, limit: u64, from_ms: u64, to_ms: u64) -> Result<Self, String> {
        match mode {
            0 => {
                if limit == 0 {
                    return Err("a message count is required".to_owned());
                }
                Ok(Range::Latest(limit.min(MAX_ROWS)))
            }
            1 => Ok(Range::All),
            2 => {
                let to_ms = if to_ms == 0 { u64::MAX } else { to_ms };
                if from_ms > to_ms {
                    return Err("the start date is after the end date".to_owned());
                }
                Ok(Range::Between { from_ms, to_ms })
            }
            _ => Err("unknown export range".to_owned()),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum StopReason {
    /// History ended at the room's creation event.
    Start,
    /// History ended earlier: the oldest this account may read.
    HistoryEnd,
    Limit,
    Date,
    Cap,
    Stalled,
}

impl StopReason {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            StopReason::Start => "start",
            StopReason::HistoryEnd => "history_end",
            StopReason::Limit => "limit",
            StopReason::Date => "date",
            StopReason::Cap => "cap",
            StopReason::Stalled => "stalled",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub(crate) struct ExportRow {
    pub event_id: String,
    pub sender: String,
    pub ts_ms: u64,
    pub kind: String,
    pub body: String,
    /// `org.matrix.custom.html` only, and untrusted: C++ sanitizes it before
    /// it reaches a file.
    pub formatted_body: String,
    pub thread_root: String,
    pub reply_to: String,
    pub edited: bool,
    pub filename: String,
    pub mimetype: String,
    pub size: u64,
    /// The content the row shows, edits applied; never serialized.
    pub content: Value,
}

impl ExportRow {
    pub(crate) fn is_attachment(&self) -> bool {
        matches!(
            self.kind.as_str(),
            "image" | "video" | "audio" | "voice" | "file" | "sticker"
        )
    }

    /// The row as the C++ side reads it. `media_key` is the opaque handle (or
    /// empty); there is deliberately no `mxc` field.
    pub(crate) fn to_json(&self, sender_name: &str, media_key: &str) -> Value {
        json!({
            "event_id": self.event_id,
            "sender": self.sender,
            "sender_name": sender_name,
            "ts_ms": self.ts_ms,
            "kind": self.kind,
            "body": self.body,
            "formatted_body": self.formatted_body,
            "thread_root": self.thread_root,
            "reply_to": self.reply_to,
            "edited": self.edited,
            "filename": self.filename,
            "mimetype": self.mimetype,
            "size": self.size,
            "media_key": media_key,
        })
    }
}

pub(crate) fn media_handle(op_id: u64, event_id: &str) -> String {
    format!("{}{op_id}:{event_id}", EXPORT_KEY_PREFIX)
}

pub(crate) const EXPORT_KEY_PREFIX: &str = "export:";

pub(crate) fn handle_prefix(op_id: u64) -> String {
    format!("{}{op_id}:", EXPORT_KEY_PREFIX)
}

#[derive(Debug, Clone, PartialEq)]
struct Edit {
    event_id: String,
    ts: u64,
    sender: String,
    new_content: Value,
    encrypted: bool,
}

#[derive(Debug)]
pub(crate) struct WalkState {
    range: Range,
    pub rows: u64,
    pub scanned: u64,
    pub undecryptable: u64,
    edits: HashMap<String, Vec<Edit>>,
    pending_edits: usize,
    /// Edits refused because `MAX_PENDING_EDITS` was full.
    pub unresolved_edits: u64,
    page_older: bool,
    page_not_older: bool,
    older_pages: u32,
    saw_create: bool,
    pub stop: Option<StopReason>,
}

fn str_of<'a>(value: Option<&'a Value>) -> &'a str {
    value.and_then(|v| v.as_str()).unwrap_or("")
}

fn formatted_of(content: &Value) -> String {
    if str_of(content.get("format")) == "org.matrix.custom.html" {
        str_of(content.get("formatted_body")).to_owned()
    } else {
        String::new()
    }
}

fn poll_text(content: &Value) -> String {
    for start_key in ["org.matrix.msc3381.poll.start", "m.poll.start", "m.poll"] {
        if let Some(question) = content.get(start_key).and_then(|s| s.get("question")) {
            for text_key in ["org.matrix.msc1767.text", "m.text", "body"] {
                let text = str_of(question.get(text_key));
                if !text.is_empty() {
                    return text.to_owned();
                }
            }
            if let Some(first) = question
                .get("m.text")
                .and_then(|t| t.as_array())
                .and_then(|a| a.first())
            {
                let text = str_of(first.get("body"));
                if !text.is_empty() {
                    return text.to_owned();
                }
            }
        }
    }
    for key in ["org.matrix.msc1767.text", "body"] {
        let text = str_of(content.get(key));
        if !text.is_empty() {
            return text.to_owned();
        }
    }
    String::new()
}

pub(crate) fn strip_reply_fallback(body: &str) -> String {
    if !body.starts_with("> ") && !body.starts_with(">\n") {
        return body.to_owned();
    }
    let mut lines = body.split('\n').peekable();
    while let Some(line) = lines.peek() {
        if line.starts_with('>') {
            lines.next();
        } else {
            break;
        }
    }
    if lines.peek().map(|l| l.is_empty()).unwrap_or(false) {
        lines.next();
    }
    lines.collect::<Vec<_>>().join("\n")
}

fn kind_for(type_str: &str, content: &Value) -> &'static str {
    match type_str {
        "m.sticker" => "sticker",
        "m.poll.start" | "org.matrix.msc3381.poll.start" => "poll",
        _ => match str_of(content.get("msgtype")) {
            "m.notice" => "notice",
            "m.emote" => "emote",
            "m.location" => "location",
            "m.image" => "image",
            "m.video" => "video",
            "m.file" => "file",
            "m.audio" => {
                if content.get("org.matrix.msc3245.voice").is_some()
                    || content.get("m.voice").is_some()
                {
                    "voice"
                } else {
                    "audio"
                }
            }
            _ => "text",
        },
    }
}

impl WalkState {
    pub(crate) fn new(range: Range) -> Self {
        Self {
            range,
            rows: 0,
            scanned: 0,
            undecryptable: 0,
            edits: HashMap::new(),
            pending_edits: 0,
            unresolved_edits: 0,
            page_older: false,
            page_not_older: false,
            older_pages: 0,
            saw_create: false,
            stop: None,
        }
    }

    fn classify(&mut self, value: &Value, encrypted: bool) -> Option<ExportRow> {
        let type_str = str_of(value.get("type"));
        let event_id = str_of(value.get("event_id")).to_owned();
        if event_id.is_empty() {
            return None;
        }
        let sender = str_of(value.get("sender")).to_owned();
        let ts_ms = value
            .get("origin_server_ts")
            .and_then(|v| v.as_u64())
            .unwrap_or(0);
        let base = ExportRow { event_id: event_id.clone(), sender: sender.clone(), ts_ms, ..Default::default() };
        if !matches!(
            type_str,
            "m.room.encrypted"
                | "m.room.message"
                | "m.sticker"
                | "m.poll.start"
                | "org.matrix.msc3381.poll.start"
        ) {
            return None;
        }
        let content = value.get("content");
        // Checked BEFORE the encrypted branch: a redacted `m.room.encrypted`
        // keeps its type with empty content, and it is a deleted message, not
        // one this device cannot read.
        let redacted = value
            .get("unsigned")
            .and_then(|u| u.get("redacted_because"))
            .is_some()
            || content
                .and_then(|c| c.as_object())
                .map(|o| o.is_empty())
                .unwrap_or(true);
        if redacted {
            // Only a server that keeps relations through redaction can tell a redacted edit from a deleted message.
            if crate::replacement::is_replacement(value) {
                return None;
            }
            return Some(ExportRow { kind: "redacted".to_owned(), ..base });
        }
        if type_str == "m.room.encrypted" {
            let rel_type = str_of(
                content
                    .and_then(|c| c.get("m.relates_to"))
                    .and_then(|r| r.get("rel_type")),
            );
            if matches!(rel_type, "m.annotation" | "m.replace") {
                return None;
            }
            return Some(ExportRow { kind: "undecryptable".to_owned(), ..base });
        }
        let content = content?;
        let relation = content.get("m.relates_to");
        let rel_type = str_of(relation.and_then(|r| r.get("rel_type")));
        if type_str == "m.room.message" && rel_type == "m.replace" {
            if let Some(r) = crate::replacement::parse(value)
                .filter(|r| crate::replacement::complete_message(r.new_content))
            {
                let candidate = Edit {
                    event_id: r.event_id.to_owned(),
                    ts: r.ts,
                    sender: sender.clone(),
                    new_content: r.new_content.clone(),
                    encrypted,
                };
                let held = self
                    .edits
                    .get_mut(r.target)
                    .and_then(|list| list.iter_mut().find(|e| e.sender == sender));
                if let Some(held) = held {
                    if crate::replacement::supersedes(
                        (candidate.ts, candidate.event_id.as_str()),
                        (held.ts, held.event_id.as_str()),
                    ) {
                        *held = candidate;
                    }
                } else if self.pending_edits < MAX_PENDING_EDITS {
                    self.edits.entry(r.target.to_owned()).or_default().push(candidate);
                    self.pending_edits += 1;
                } else {
                    self.unresolved_edits += 1;
                }
            }
            return None;
        }

        // Only the original sender's edit replaces content (the spec's rule): an
        // edit by anyone else is ignored, never applied.
        let mut edited = false;
        let mut projected = None;
        if let Some(list) = self.edits.remove(&event_id) {
            self.pending_edits = self.pending_edits.saturating_sub(list.len());
            if let Some(edit) = list.into_iter().find(|e| {
                value.get("state_key").is_none()
                    && crate::replacement::may_replace(&e.sender, "m.room.message", &sender, type_str)
                    && crate::replacement::encryption_permits(e.encrypted, encrypted)
            }) {
                projected = Some(crate::replacement::project(content, &edit.new_content));
                edited = true;
            }
        }
        let content = projected.as_ref().unwrap_or(content);

        let kind = kind_for(type_str, content);
        let thread_root = if rel_type == "m.thread" {
            str_of(relation.and_then(|r| r.get("event_id"))).to_owned()
        } else {
            String::new()
        };
        // In a thread, `m.in_reply_to` with `is_falling_back` is only there for
        // clients without threads; it is not a reply the sender made.
        let falling_back = relation
            .and_then(|r| r.get("is_falling_back"))
            .and_then(|v| v.as_bool())
            .unwrap_or(false);
        let reply_to = if falling_back {
            String::new()
        } else {
            str_of(relation.and_then(|r| r.get("m.in_reply_to")).and_then(|r| r.get("event_id")))
                .to_owned()
        };

        let mut body = if kind == "poll" {
            poll_text(content)
        } else {
            str_of(content.get("body")).to_owned()
        };
        let formatted_body = formatted_of(content);
        if !reply_to.is_empty() && !edited {
            body = strip_reply_fallback(&body);
        }

        let mut row = ExportRow {
            kind: kind.to_owned(),
            thread_root,
            reply_to,
            edited,
            ..base
        };
        if row.is_attachment() {
            let info = content.get("info");
            row.filename = content
                .get("filename")
                .and_then(|v| v.as_str())
                .map(str::to_owned)
                .unwrap_or_else(|| str_of(content.get("body")).to_owned());
            row.mimetype = str_of(info.and_then(|i| i.get("mimetype"))).to_owned();
            row.size = info
                .and_then(|i| i.get("size"))
                .and_then(|v| v.as_u64())
                .unwrap_or(0);
        }
        row.body = body;
        row.formatted_body = formatted_body;
        row.content = content.clone();
        Some(row)
    }

    /// `encrypted`: whether this event arrived encrypted.
    pub(crate) fn consume(&mut self, value: &Value, encrypted: bool) -> Option<ExportRow> {
        if self.stop.is_some() {
            return None;
        }
        self.scanned += 1;
        if str_of(value.get("type")) == "m.room.create" && str_of(value.get("state_key")).is_empty() {
            self.saw_create = true;
        }
        let row = self.classify(value, encrypted);
        let ts = value
            .get("origin_server_ts")
            .and_then(|v| v.as_u64())
            .unwrap_or(0);
        if let Range::Between { from_ms, to_ms } = self.range {
            if ts != 0 {
                if ts < from_ms {
                    self.page_older = true;
                } else {
                    self.page_not_older = true;
                }
            }
            if ts < from_ms || ts > to_ms {
                return None;
            }
        }
        let row = row?;
        if row.kind == "undecryptable" {
            self.undecryptable += 1;
        }
        self.rows += 1;
        if let Range::Latest(n) = self.range {
            if self.rows >= n {
                self.stop = Some(StopReason::Limit);
            }
        }
        if self.stop.is_none() && self.rows >= MAX_ROWS {
            self.stop = Some(StopReason::Cap);
        }
        Some(row)
    }

    pub(crate) fn end_of_page(&mut self, end: PageEnd) {
        if self.page_not_older {
            self.older_pages = 0;
        } else if self.page_older {
            self.older_pages += 1;
        }
        self.page_older = false;
        self.page_not_older = false;
        if self.stop.is_some() {
            return;
        }
        self.stop = match end {
            PageEnd::End if self.saw_create => Some(StopReason::Start),
            PageEnd::End => Some(StopReason::HistoryEnd),
            PageEnd::Stalled => Some(StopReason::Stalled),
            PageEnd::More if self.older_pages >= DATE_GRACE_PAGES => Some(StopReason::Date),
            PageEnd::More => None,
        };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(id: &str, sender: &str, ts: u64, body: &str) -> Value {
        json!({
            "type": "m.room.message",
            "event_id": id,
            "sender": sender,
            "origin_server_ts": ts,
            "content": { "msgtype": "m.text", "body": body },
        })
    }

    fn edit(id: &str, sender: &str, ts: u64, target: &str, body: &str) -> Value {
        json!({
            "type": "m.room.message",
            "event_id": id,
            "sender": sender,
            "origin_server_ts": ts,
            "content": {
                "msgtype": "m.text",
                "body": format!("* {body}"),
                "m.new_content": {
                    "msgtype": "m.text",
                    "body": body,
                    "format": "org.matrix.custom.html",
                    "formatted_body": format!("<b>{body}</b>"),
                },
                "m.relates_to": { "rel_type": "m.replace", "event_id": target },
            },
        })
    }

    fn rows_of(state: &mut WalkState, page: &[Value]) -> Vec<ExportRow> {
        page.iter().filter_map(|v| state.consume(v, false)).collect()
    }

    #[test]
    fn room_export_matches_derived_fixtures() {
        use crate::derived_fixtures::{scenarios, Projection, ORIGINAL, OTHER_ROOM, ROOM};

        for case in scenarios() {
            let mut rows = Vec::new();
            for room in [ROOM, OTHER_ROOM] {
                let mut walk = WalkState::new(Range::All);
                for (event, encrypted) in case.history(room) {
                    rows.extend(walk.consume(event, encrypted));
                }
            }
            let want = case.content_for(Projection::Export);
            assert_eq!(rows.len(), usize::from(want.is_some()), "{}", case.name);
            if let Some(content) = want {
                let row = &rows[0];
                assert_eq!(row.event_id, ORIGINAL, "{}", case.name);
                assert_eq!(&row.content, content, "{}", case.name);
                let body = content["body"].as_str().unwrap();
                assert_eq!(row.body, body, "{}", case.name);
                assert_eq!(row.edited, case.original.as_ref().unwrap()["content"] != *content, "{}", case.name);
                assert_eq!(row.is_attachment(), content.get("url").is_some(), "{}", case.name);
                if row.is_attachment() {
                    assert_eq!(row.filename, content["filename"].as_str().unwrap_or(body), "{}", case.name);
                }
            }
        }
    }

    #[test]
    fn only_the_original_senders_newest_edit_replaces_the_text() {
        let mut state = WalkState::new(Range::All);
        let page = vec![
            edit("$e3", "@mallory:example.org", 400, "$1", "forged"),
            edit("$e2", "@ann:example.org", 300, "$1", "second"),
            edit("$e1", "@ann:example.org", 200, "$1", "first"),
            text("$1", "@ann:example.org", 100, "original"),
        ];
        let rows = rows_of(&mut state, &page);
        assert_eq!(rows.len(), 1, "an edit must never be a row of its own");
        let row = &rows[0];
        assert_eq!(row.event_id, "$1");
        assert_eq!(row.body, "second");
        assert_eq!(row.formatted_body, "<b>second</b>");
        assert!(row.edited);
    }

    #[test]
    fn the_newest_edit_wins_by_time_then_event_id_not_by_walk_order() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            edit("$e1", "@ann:example.org", 200, "$1", "first"),
            edit("$e2", "@ann:example.org", 300, "$1", "second"),
            text("$1", "@ann:example.org", 100, "original"),
        ]);
        assert_eq!(rows[0].body, "second");
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            edit("$b", "@ann:example.org", 200, "$1", "bee"),
            edit("$a", "@ann:example.org", 200, "$1", "ay"),
            text("$1", "@ann:example.org", 100, "original"),
        ]);
        assert_eq!(rows[0].body, "bee");
    }

    #[test]
    fn a_plaintext_edit_cannot_rewrite_an_encrypted_message() {
        for (edit_encrypted, expected) in [(false, "original"), (true, "second")] {
            let mut state = WalkState::new(Range::All);
            assert!(state.consume(&edit("$e", "@ann:example.org", 200, "$1", "second"), edit_encrypted).is_none());
            let row = state.consume(&text("$1", "@ann:example.org", 100, "original"), true).unwrap();
            assert_eq!(row.body, expected, "edit encrypted: {edit_encrypted}");
        }
    }

    #[test]
    fn a_message_edit_cannot_rewrite_a_sticker() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            edit("$e1", "@ann:example.org", 200, "$s", "rewritten"),
            json!({"type": "m.sticker", "event_id": "$s", "sender": "@ann:example.org",
                   "origin_server_ts": 100u64,
                   "content": {"body": "waving cat", "url": "mxc://example.org/cat"}}),
        ]);
        assert_eq!(rows[0].body, "waving cat");
        assert!(!rows[0].edited);
    }

    #[test]
    fn an_edit_by_someone_else_alone_changes_nothing() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            edit("$e1", "@mallory:example.org", 200, "$1", "forged"),
            text("$1", "@ann:example.org", 100, "original"),
        ]);
        assert_eq!(rows[0].body, "original");
        assert!(!rows[0].edited);
    }

    #[test]
    fn a_redacted_message_is_a_deleted_row_with_no_content() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.message",
            "event_id": "$r",
            "sender": "@ann:example.org",
            "origin_server_ts": 5u64,
            "content": {},
            "unsigned": { "redacted_because": { "type": "m.room.redaction" } },
        })]);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].kind, "redacted");
        assert!(rows[0].body.is_empty());
        assert!(rows[0].formatted_body.is_empty());
    }

    #[test]
    fn a_thread_reply_is_flagged_and_its_fallback_reply_is_not_a_reply() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.message",
            "event_id": "$t",
            "sender": "@ann:example.org",
            "origin_server_ts": 9u64,
            "content": {
                "msgtype": "m.text",
                "body": "in the thread",
                "m.relates_to": {
                    "rel_type": "m.thread",
                    "event_id": "$root",
                    "is_falling_back": true,
                    "m.in_reply_to": { "event_id": "$latest" },
                },
            },
        }), json!({
            "type": "m.room.message",
            "event_id": "$reply",
            "sender": "@bob:example.org",
            "origin_server_ts": 8u64,
            "content": {
                "msgtype": "m.text",
                "body": "> <@ann:example.org> quoted\n> more\n\nthe answer",
                "m.relates_to": { "m.in_reply_to": { "event_id": "$q" } },
            },
        })]);
        assert_eq!(rows[0].thread_root, "$root");
        assert!(rows[0].reply_to.is_empty());
        assert_eq!(rows[1].thread_root, "");
        assert_eq!(rows[1].reply_to, "$q");
        assert_eq!(rows[1].body, "the answer", "the quoted fallback stays out");
    }

    #[test]
    fn an_attachment_row_carries_a_handle_and_never_a_url() {
        let mut state = WalkState::new(Range::All);
        let event = json!({
            "type": "m.room.message",
            "event_id": "$img",
            "sender": "@ann:example.org",
            "origin_server_ts": 7u64,
            "content": {
                "msgtype": "m.image",
                "body": "holiday.png",
                "url": "mxc://example.org/secretmedia",
                "info": {
                    "mimetype": "image/png",
                    "size": 1234,
                    "thumbnail_url": "mxc://example.org/thumb",
                },
            },
        });
        let rows = rows_of(&mut state, &[event.clone()]);
        let row = &rows[0];
        assert_eq!(row.kind, "image");
        assert!(row.is_attachment());
        assert_eq!(row.filename, "holiday.png");
        assert_eq!(row.mimetype, "image/png");
        assert_eq!(row.size, 1234);
        assert!(crate::mediahistory::stored_media_from_event(&event).is_some());
        let handle = media_handle(42, &row.event_id);
        assert!(handle.starts_with(&handle_prefix(42)));
        let serialized = row.to_json("Ann", &handle).to_string();
        assert!(!serialized.contains("mxc://"), "{serialized}");
        assert!(!serialized.contains("http"), "{serialized}");
        assert!(serialized.contains("export:42:$img"));
    }

    #[test]
    fn an_unreadable_event_is_a_counted_row() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.encrypted",
            "event_id": "$u",
            "sender": "@ann:example.org",
            "origin_server_ts": 3u64,
            "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x" },
        })]);
        assert_eq!(rows[0].kind, "undecryptable");
        assert_eq!(state.undecryptable, 1);
    }

    #[test]
    fn a_redacted_encrypted_event_is_deleted_not_undecryptable() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.encrypted",
            "event_id": "$gone",
            "sender": "@ann:example.org",
            "origin_server_ts": 6u64,
            "content": {},
            "unsigned": { "redacted_because": { "type": "m.room.redaction" } },
        })]);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].kind, "redacted");
        assert_eq!(state.undecryptable, 0);
    }

    #[test]
    fn an_unreadable_reaction_or_edit_is_not_a_row_and_not_counted() {
        let mut state = WalkState::new(Range::Latest(2));
        let encrypted = |id: &str, rel: &str| json!({
            "type": "m.room.encrypted",
            "event_id": id,
            "sender": "@ann:example.org",
            "origin_server_ts": 10u64,
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "x",
                "m.relates_to": { "rel_type": rel, "event_id": "$target" },
            },
        });
        let rows = rows_of(&mut state, &[
            encrypted("$r", "m.annotation"),
            encrypted("$e", "m.replace"),
            text("$1", "@a:b", 9, "one"),
            text("$2", "@a:b", 8, "two"),
        ]);
        let ids: Vec<&str> = rows.iter().map(|r| r.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$1", "$2"], "relations used up the latest-N budget");
        assert_eq!(state.undecryptable, 0);
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[encrypted("$t", "m.thread")]);
        assert_eq!(rows[0].kind, "undecryptable");
        assert_eq!(state.undecryptable, 1);
    }

    #[test]
    fn state_events_reactions_and_redaction_events_are_not_rows() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            json!({ "type": "m.room.member", "event_id": "$m", "sender": "@a:b",
                    "origin_server_ts": 1u64, "state_key": "@a:b",
                    "content": { "membership": "join" } }),
            json!({ "type": "m.reaction", "event_id": "$x", "sender": "@a:b",
                    "origin_server_ts": 1u64, "content": {} }),
            json!({ "type": "m.room.redaction", "event_id": "$y", "sender": "@a:b",
                    "origin_server_ts": 1u64, "redacts": "$z", "content": {} }),
        ]);
        assert!(rows.is_empty());
        assert_eq!(state.scanned, 3);
    }

    #[test]
    fn latest_n_stops_at_exactly_n_rows() {
        let mut state = WalkState::new(Range::Latest(3));
        let page: Vec<Value> = (0..10)
            .map(|i| text(&format!("${i}"), "@a:b", 1000 - i, "hi"))
            .collect();
        let rows = rows_of(&mut state, &page);
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].event_id, "$0", "the NEWEST rows are kept");
        assert_eq!(state.stop, Some(StopReason::Limit));
        assert!(state.consume(&text("$x", "@a:b", 1, "late"), false).is_none());
        state.end_of_page(PageEnd::End);
        assert_eq!(state.stop, Some(StopReason::Limit));
    }

    #[test]
    fn a_date_range_keeps_its_days_and_ends_after_the_page_that_passes_it() {
        let mut state = WalkState::new(Range::Between { from_ms: 100, to_ms: 200 });
        let rows = rows_of(&mut state, &[
            text("$new", "@a:b", 250, "too new"),
            text("$in1", "@a:b", 200, "edge"),
            text("$in2", "@a:b", 150, "middle"),
            text("$old", "@a:b", 99, "too old"),
            text("$skew", "@a:b", 120, "skewed clock"),
        ]);
        let ids: Vec<&str> = rows.iter().map(|r| r.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$in1", "$in2", "$skew"]);
        state.end_of_page(PageEnd::More);
        assert_eq!(state.stop, None, "a page with days in range is not past the range");
        for page in 0..DATE_GRACE_PAGES {
            assert_eq!(state.stop, None, "stopped after {page} old pages");
            rows_of(&mut state, &[text(&format!("$o{page}"), "@a:b", 50, "old")]);
            state.end_of_page(PageEnd::More);
        }
        assert_eq!(state.stop, Some(StopReason::Date));
    }

    #[test]
    fn an_old_timestamp_on_one_page_does_not_hide_a_later_page_in_range() {
        let mut state = WalkState::new(Range::Between { from_ms: 100, to_ms: 200 });
        let mut kept = rows_of(&mut state, &[text("$a", "@a:b", 150, "in"), text("$b", "@a:b", 50, "skewed")]);
        state.end_of_page(PageEnd::More);
        kept.extend(rows_of(&mut state, &[
            json!({ "type": "m.room.member", "event_id": "$m", "sender": "@a:b",
                    "origin_server_ts": 10u64, "state_key": "@a:b",
                    "content": { "membership": "join" } }),
        ]));
        state.end_of_page(PageEnd::More);
        kept.extend(rows_of(&mut state, &[text("$c", "@a:b", 130, "in")]));
        state.end_of_page(PageEnd::End);
        let ids: Vec<&str> = kept.iter().map(|r| r.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$a", "$c"]);
        assert_eq!(state.stop, Some(StopReason::HistoryEnd));
    }

    #[test]
    fn the_start_of_the_room_ends_an_unbounded_walk() {
        let mut state = WalkState::new(Range::All);
        rows_of(&mut state, &[text("$1", "@a:b", 1, "first ever")]);
        state.end_of_page(PageEnd::More);
        assert_eq!(state.stop, None);
        rows_of(&mut state, &[json!({ "type": "m.room.create", "event_id": "$c", "sender": "@a:b",
                                      "origin_server_ts": 1u64, "state_key": "",
                                      "content": { "room_version": "11" } })]);
        state.end_of_page(PageEnd::End);
        assert_eq!(state.stop, Some(StopReason::Start));
    }

    #[test]
    fn history_end_stall_and_cap_are_distinct_reasons() {
        let mut state = WalkState::new(Range::All);
        state.end_of_page(PageEnd::End);
        assert_eq!(state.stop, Some(StopReason::HistoryEnd), "no create event: not the start of the room");
        let mut state = WalkState::new(Range::All);
        state.end_of_page(PageEnd::Stalled);
        assert_eq!(state.stop, Some(StopReason::Stalled));
        let reasons: HashSet<&str> = [
            StopReason::Start, StopReason::HistoryEnd, StopReason::Limit,
            StopReason::Date, StopReason::Cap, StopReason::Stalled,
        ].iter().map(|r| r.as_str()).collect();
        assert_eq!(reasons.len(), 6);
    }

    #[test]
    fn an_empty_page_with_a_token_continues_and_only_an_absent_token_ends() {
        let mut pager = Pager::default();
        assert_eq!(pager.after_page(None, Some("t1"), 0), PageEnd::More);
        assert_eq!(pager.after_page(Some("t1"), Some("t2"), 5), PageEnd::More);
        assert_eq!(pager.after_page(Some("t2"), None, 0), PageEnd::End);
    }

    #[test]
    fn a_repeated_token_a_cycle_or_endless_empty_pages_is_a_stall() {
        let mut pager = Pager::default();
        assert_eq!(pager.after_page(Some("t1"), Some("t1"), 0), PageEnd::Stalled);
        let mut pager = Pager::default();
        assert_eq!(pager.after_page(Some("a"), Some("b"), 3), PageEnd::More);
        assert_eq!(pager.after_page(Some("b"), Some("a"), 3), PageEnd::Stalled);
        let mut pager = Pager::default();
        let mut from = "t0".to_owned();
        let mut last = PageEnd::More;
        for i in 1..=MAX_EMPTY_PAGES {
            let end = format!("t{i}");
            last = pager.after_page(Some(&from), Some(&end), 0);
            from = end;
        }
        assert_eq!(last, PageEnd::Stalled);
    }

    fn file_event(id: &str, name: &str, url: &str) -> Value {
        json!({
            "type": "m.room.message", "event_id": id, "sender": "@ann:example.org",
            "origin_server_ts": 100u64,
            "content": { "msgtype": "m.file", "body": name, "url": url,
                         "info": { "mimetype": "application/pdf", "size": 10 } },
        })
    }

    fn replacing(id: &str, target: &str, ts: u64, new_content: Value) -> Value {
        json!({
            "type": "m.room.message", "event_id": id, "sender": "@ann:example.org",
            "origin_server_ts": ts,
            "content": { "msgtype": "m.text", "body": "* edited", "m.new_content": new_content,
                         "m.relates_to": { "rel_type": "m.replace", "event_id": target } },
        })
    }

    #[test]
    fn an_edit_replaces_the_whole_content_including_the_attachment() {
        let mut state = WalkState::new(Range::All);
        let row = rows_of(&mut state, &[
            replacing("$e", "$f", 200, json!({ "msgtype": "m.text", "body": "never mind" })),
            file_event("$f", "a.pdf", "mxc://example.org/a"),
        ]).remove(0);
        assert_eq!((row.kind.as_str(), row.body.as_str()), ("text", "never mind"));
        assert!(!row.is_attachment());
        assert!(crate::mediahistory::stored_media_from_content(&row.content).is_none());

        let mut state = WalkState::new(Range::All);
        let row = rows_of(&mut state, &[
            replacing("$e", "$f", 200, json!({ "msgtype": "m.file", "body": "b.pdf",
                                                "url": "mxc://example.org/b",
                                                "info": { "mimetype": "text/plain", "size": 99 } })),
            file_event("$f", "a.pdf", "mxc://example.org/a"),
        ]).remove(0);
        assert_eq!((row.filename.as_str(), row.mimetype.as_str(), row.size), ("b.pdf", "text/plain", 99));
        let media = crate::mediahistory::stored_media_from_content(&row.content).unwrap();
        assert!(matches!(&media.source,
            matrix_sdk::ruma::events::room::MediaSource::Plain(uri) if uri.as_str() == "mxc://example.org/b"));

        let mut state = WalkState::new(Range::All);
        let row = rows_of(&mut state, &[
            replacing("$e", "$1", 200, json!({ "msgtype": "m.emote", "body": "waves" })),
            text("$1", "@ann:example.org", 100, "hello"),
        ]).remove(0);
        assert_eq!((row.kind.as_str(), row.body.as_str()), ("emote", "waves"));
    }

    #[test]
    fn an_incomplete_edit_is_ignored_and_the_original_relations_stay() {
        let mut state = WalkState::new(Range::All);
        let mut original = text("$1", "@ann:example.org", 100, "in the thread");
        original["content"]["m.relates_to"] = json!({ "rel_type": "m.thread", "event_id": "$root" });
        let row = rows_of(&mut state, &[
            replacing("$bad", "$1", 300, json!({ "msgtype": "m.image", "body": "no file" })),
            replacing("$good", "$1", 200, json!({ "msgtype": "m.text", "body": "fixed",
                "m.relates_to": { "rel_type": "m.thread", "event_id": "$elsewhere" } })),
            original,
        ]).remove(0);
        assert_eq!(row.body, "fixed");
        assert_eq!(row.thread_root, "$root");
    }

    #[test]
    fn eight_strangers_cannot_crowd_out_the_authors_edit() {
        let mut state = WalkState::new(Range::All);
        let mut page: Vec<Value> = (0..8)
            .map(|i| edit(&format!("$x{i}"), &format!("@m{i}:example.org"), 500, "$1", "forged"))
            .collect();
        page.push(edit("$own", "@ann:example.org", 200, "$1", "corrected"));
        page.push(text("$1", "@ann:example.org", 100, "original"));
        let rows = rows_of(&mut state, &page);
        assert_eq!(rows[0].body, "corrected");
        assert_eq!(state.unresolved_edits, 0);
    }

    #[test]
    fn the_row_cap_holds_for_every_range() {
        let mut state = WalkState::new(Range::All);
        state.rows = MAX_ROWS - 1;
        assert!(state.consume(&text("$1", "@a:b", 1, "x"), false).is_some());
        assert_eq!(state.stop, Some(StopReason::Cap));
        assert_eq!(Range::from_ffi(0, 10 * MAX_ROWS, 0, 0), Ok(Range::Latest(MAX_ROWS)));
    }

    #[test]
    fn ffi_ranges_are_validated() {
        assert!(Range::from_ffi(0, 0, 0, 0).is_err());
        assert!(Range::from_ffi(2, 0, 500, 100).is_err());
        assert_eq!(
            Range::from_ffi(2, 0, 500, 0),
            Ok(Range::Between { from_ms: 500, to_ms: u64::MAX })
        );
        assert!(Range::from_ffi(7, 0, 0, 0).is_err());
    }

    #[test]
    fn a_poll_exports_its_question() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "org.matrix.msc3381.poll.start",
            "event_id": "$p",
            "sender": "@a:b",
            "origin_server_ts": 4u64,
            "content": {
                "org.matrix.msc3381.poll.start": {
                    "question": { "org.matrix.msc1767.text": "Lunch?" },
                    "answers": [],
                },
            },
        })]);
        assert_eq!(rows[0].kind, "poll");
        assert_eq!(rows[0].body, "Lunch?");
    }

    #[test]
    fn a_reply_fallback_is_stripped_only_from_its_quote() {
        assert_eq!(strip_reply_fallback("> <@a:b> hi\n\nanswer"), "answer");
        assert_eq!(strip_reply_fallback("plain"), "plain");
        assert_eq!(strip_reply_fallback("> only quote"), "");
    }
}
