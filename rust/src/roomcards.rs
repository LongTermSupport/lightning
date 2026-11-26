//! Shared checklists and community events: an `m.room.message` with a custom
//! msgtype and a text `body` other clients show, plus reference-related
//! updates (`checklist.update`, `rsvp`) that every reader folds in the same
//! (origin_server_ts, event_id) order, so all clients converge.

use std::time::Duration;

use matrix_sdk::{
    room::{IncludeRelations, RelationsOptions},
    ruma::{
        api::Direction,
        events::{room::power_levels::RoomPowerLevels, MessageLikeEventType},
        EventId, UInt, UserId,
    },
    Room,
};
use serde_json::{json, Value};

use crate::community::{digest, err, https_link, text};

pub(crate) const CHECKLIST: &str = "org.lightning_matrix.checklist";
pub(crate) const EVENT: &str = "org.lightning_matrix.event";
const CHECKLIST_UPDATE: &str = "org.lightning_matrix.checklist.update";
const RSVP: &str = "org.lightning_matrix.rsvp";
const MAX_ITEMS: usize = 100;
const MAX_ITEM_TEXT: usize = 300;
const MAX_TITLE: usize = 200;
const MAX_FALLBACK: usize = 8000;
const MAX_RELATED: usize = 2000;
const MAX_GOING_LISTED: usize = 50;
const YEAR_MS: i64 = 366 * 24 * 3600 * 1000;

type Result<T> = std::result::Result<T, String>;

fn clamp(s: &str, max: usize) -> String {
    s.chars().take(max).collect()
}

fn bounded(v: &Value, key: &str, max: usize) -> bool {
    v.get(key).and_then(Value::as_str).is_some_and(|s| s.chars().count() <= max)
}

fn optional(v: &Value, key: &str, max: usize) -> bool {
    v.get(key).is_none() || bounded(v, key, max)
}

/// The card kind of a msgtype Lightning renders as a card, if any.
pub(crate) fn card_kind(msgtype: &str) -> Option<&'static str> {
    match msgtype {
        CHECKLIST => Some("checklist"),
        EVENT => Some("event"),
        _ => None,
    }
}

fn valid_checklist(v: &Value) -> bool {
    optional(v, "title", MAX_TITLE)
        && v.get("anyone_edits").is_none_or(Value::is_boolean)
        && v.get("items").and_then(Value::as_array).is_some_and(|items| {
            let mut ids = std::collections::HashSet::new();
            !items.is_empty()
                && items.len() <= MAX_ITEMS
                && items.iter().all(|i| {
                    let id = text(i, "id");
                    !id.is_empty()
                        && id.len() <= 64
                        && ids.insert(id.to_owned())
                        && bounded(i, "text", MAX_ITEM_TEXT)
                        && !text(i, "text").trim().is_empty()
                })
        })
}

fn valid_event(v: &Value) -> bool {
    let start = v.get("start").and_then(Value::as_i64).unwrap_or(0);
    let end = v.get("end").map_or(Some(0), Value::as_i64).unwrap_or(-1);
    bounded(v, "title", MAX_TITLE)
        && !text(v, "title").trim().is_empty()
        && start > 0
        && (end == 0 || (end > start && end - start <= YEAR_MS))
        && optional(v, "tz", 64)
        && optional(v, "location", MAX_ITEM_TEXT)
        && (text(v, "link").is_empty() || https_link(text(v, "link")))
        && v.get("cancelled").is_none_or(Value::is_boolean)
}

fn valid(kind: &str, v: &Value) -> bool {
    match kind {
        CHECKLIST => valid_checklist(v),
        EVENT => valid_event(v),
        _ => false,
    }
}

fn relation<'a>(e: &'a Value) -> (&'a str, &'a str) {
    let r = &e["content"]["m.relates_to"];
    (text(r, "rel_type"), text(r, "event_id"))
}

fn ordered(related: &[Value]) -> Vec<&Value> {
    let mut out: Vec<&Value> = related.iter().collect();
    out.sort_by(|a, b| {
        let key = |e: &Value| (e["origin_server_ts"].as_u64().unwrap_or(0), text(e, "event_id").to_owned());
        key(*a).cmp(&key(*b))
    });
    out.dedup_by(|a, b| text(*a, "event_id") == text(*b, "event_id"));
    out
}

/// The card's current content: the root's, replaced by its sender's latest valid edit.
/// `None` when the root is redacted or carries nothing valid.
fn current(root: &Value, related: &[Value]) -> Option<(&'static str, Value)> {
    let kind = [CHECKLIST, EVENT].into_iter().find(|k| text(&root["content"], "msgtype") == *k)?;
    let sender = text(root, "sender");
    let root_id = text(root, "event_id");
    let mut content = root["content"].get(kind).filter(|v| valid(kind, v)).cloned();
    for e in ordered(related) {
        let (rel, target) = relation(e);
        if rel != "m.replace" || target != root_id || text(e, "sender") != sender {
            continue;
        }
        let new = &e["content"]["m.new_content"];
        if text(new, "msgtype") == kind {
            if let Some(v) = new.get(kind).filter(|v| valid(kind, v)) {
                content = Some(v.clone());
            }
        }
    }
    content.map(|c| (kind, c))
}

/// Folds checklist updates in (ts, id) order. Only the creator, moderators, or anyone
/// when the creator allowed it, may change the list; others' updates are ignored.
pub(crate) fn checklist_state(root: &Value, related: &[Value], may_moderate: &dyn Fn(&str) -> bool) -> Value {
    let Some((CHECKLIST, list)) = current(root, related) else {
        return json!({ "kind": "checklist", "deleted": true });
    };
    let creator = text(root, "sender");
    let root_id = text(root, "event_id");
    let anyone = list["anyone_edits"].as_bool().unwrap_or(true);
    let mut items: Vec<Value> = list["items"]
        .as_array()
        .into_iter()
        .flatten()
        .map(|i| json!({ "id": text(i, "id"), "text": clamp(text(i, "text"), MAX_ITEM_TEXT),
            "done": false, "by": "", "at": 0, "added_by": creator }))
        .collect();
    let mut removed: Vec<String> = Vec::new();
    for e in ordered(related) {
        let (rel, target) = relation(e);
        if text(e, "type") != CHECKLIST_UPDATE || rel != "m.reference" || target != root_id {
            continue;
        }
        let sender = text(e, "sender");
        if !(anyone || sender == creator || may_moderate(sender)) {
            continue;
        }
        let c = &e["content"];
        let item = text(c, "item");
        let at = e["origin_server_ts"].as_u64().unwrap_or(0);
        match text(c, "op") {
            "add" => {
                let id = text(e, "event_id");
                let body = text(c, "text");
                if items.len() < MAX_ITEMS && !body.trim().is_empty() && body.chars().count() <= MAX_ITEM_TEXT
                    && !items.iter().any(|i| i["id"] == id)
                {
                    items.push(json!({ "id": id, "text": body, "done": false, "by": "", "at": 0,
                        "added_by": sender }));
                }
            }
            op @ ("done" | "undone") => {
                if let Some(i) = items.iter_mut().find(|i| i["id"] == item) {
                    i["done"] = json!(op == "done");
                    i["by"] = json!(sender);
                    i["at"] = json!(at);
                }
            }
            "remove" => removed.push(item.to_owned()),
            _ => {}
        }
    }
    items.retain(|i| !removed.iter().any(|r| i["id"] == r.as_str()));
    let done = items.iter().filter(|i| i["done"] == true).count();
    json!({ "kind": "checklist", "deleted": false, "title": clamp(text(&list, "title"), MAX_TITLE),
        "creator": creator, "anyone_edits": anyone, "items": items, "done_count": done })
}

/// The latest RSVP per member wins; RSVPs are counted even after a cancellation
/// so un-cancelling restores them.
pub(crate) fn event_state(root: &Value, related: &[Value], own: &str) -> Value {
    let Some((EVENT, ev)) = current(root, related) else {
        return json!({ "kind": "event", "deleted": true });
    };
    let root_id = text(root, "event_id");
    let mut latest: std::collections::BTreeMap<String, String> = Default::default();
    for e in ordered(related) {
        let (rel, target) = relation(e);
        let status = text(&e["content"], "status");
        if text(e, "type") == RSVP && rel == "m.reference" && target == root_id
            && ["yes", "no", "maybe"].contains(&status)
        {
            latest.insert(text(e, "sender").to_owned(), status.to_owned());
        }
    }
    let count = |s: &str| latest.values().filter(|v| v.as_str() == s).count();
    let going: Vec<&String> = latest.iter().filter(|(_, s)| s.as_str() == "yes").map(|(u, _)| u)
        .take(MAX_GOING_LISTED).collect();
    json!({ "kind": "event", "deleted": false, "creator": text(root, "sender"),
        "title": clamp(text(&ev, "title"), MAX_TITLE), "start": ev["start"], "end": ev["end"].as_i64().unwrap_or(0),
        "tz": text(&ev, "tz"), "location": text(&ev, "location"), "link": text(&ev, "link"),
        "cancelled": ev["cancelled"].as_bool().unwrap_or(false),
        "yes": count("yes"), "no": count("no"), "maybe": count("maybe"),
        "own": latest.get(own).cloned().unwrap_or_default(), "going": going })
}

fn event_json(event: &matrix_sdk::deserialized_responses::TimelineEvent) -> Option<Value> {
    serde_json::from_str(event.raw().json().get()).ok()
}

async fn related(room: &Room, root: &EventId, server: bool) -> Result<Vec<Value>> {
    let mut out = Vec::new();
    if server {
        let mut from = None;
        while out.len() < MAX_RELATED {
            let opts = RelationsOptions {
                from,
                dir: Direction::Backward,
                limit: Some(UInt::from(100u32)),
                include_relations: IncludeRelations::AllRelations,
                recurse: false,
            };
            let page = tokio::time::timeout(Duration::from_secs(20), room.relations(root.to_owned(), opts))
                .await
                .map_err(|_| "unknown".to_owned())?
                .map_err(err)?;
            out.extend(page.chunk.iter().filter_map(event_json));
            from = page.next_batch_token;
            if from.is_none() {
                break;
            }
        }
    }
    if let Ok((cache, _handles)) = room.event_cache().await {
        if let Ok(local) = cache.find_event_relations(root, None).await {
            out.extend(local.iter().filter_map(event_json));
        }
    }
    out.truncate(MAX_RELATED * 2);
    Ok(out)
}

async fn root_event(room: &Room, root: &EventId) -> Result<Value> {
    let event = room.load_or_fetch_event(root, None).await.map_err(err)?;
    event_json(&event).ok_or_else(|| "invalid".to_owned())
}

fn levels_allow(levels: Option<&RoomPowerLevels>, user: &str, ty: &str) -> bool {
    UserId::parse(user).is_ok_and(|u| {
        levels.is_some_and(|l| l.user_can_send_message(&u, MessageLikeEventType::from(ty)))
    })
}

async fn load(room: &Room, own: &UserId, root: &EventId, server: bool) -> Result<Value> {
    let root_ev = root_event(room, root).await?;
    let rel = related(room, root, server).await?;
    let levels = room.power_levels().await.ok();
    let moderator = |u: &str| {
        UserId::parse(u).is_ok_and(|u| levels.as_ref().is_some_and(|l| l.user_can_redact_event_of_other(&u)))
    };
    let mut out = match text(&root_ev["content"], "msgtype") {
        CHECKLIST => checklist_state(&root_ev, &rel, &moderator),
        EVENT => event_state(&root_ev, &rel, own.as_str()),
        _ => return Err("invalid".into()),
    };
    let creator = out["creator"].as_str().unwrap_or("").to_owned();
    let anyone = out["anyone_edits"].as_bool().unwrap_or(false);
    let own_s = own.as_str();
    out["root"] = json!(root.as_str());
    out["can_edit"] = json!(!creator.is_empty() && creator == own_s);
    out["can_update"] = json!(
        (anyone || creator == own_s || moderator(own_s))
            && levels_allow(levels.as_ref(), own_s, CHECKLIST_UPDATE)
    );
    out["can_rsvp"] = json!(levels_allow(levels.as_ref(), own_s, RSVP));
    Ok(out)
}

fn root_id(payload: &Value) -> Result<matrix_sdk::ruma::OwnedEventId> {
    EventId::parse(text(payload, "root")).map_err(|_| "invalid".to_owned())
}

fn reference(root: &EventId) -> Value {
    json!({ "rel_type": "m.reference", "event_id": root.as_str() })
}

pub(crate) async fn run(room: &Room, own: &UserId, action: &str, payload: &Value) -> Result<Value> {
    match action {
        "card_load" => load(room, own, &root_id(payload)?, payload["server"].as_bool().unwrap_or(false)).await,
        "card_create" => {
            let kind = text(payload, "kind");
            let data = &payload["data"];
            let body = text(payload, "body");
            if !valid(kind, data) || body.trim().is_empty() || body.chars().count() > MAX_FALLBACK {
                return Err("invalid".into());
            }
            let levels = room.power_levels().await.ok();
            if !levels_allow(levels.as_ref(), own.as_str(), "m.room.message") {
                return Err("forbidden".into());
            }
            // A retried create with the same nonce reuses its transaction id and posts once.
            let txn = format!("lightning-card-{}", digest(&json!([kind, data, body, own.as_str(), text(payload, "nonce")])));
            let sent = room
                .send_raw("m.room.message", json!({ "msgtype": kind, "body": body, kind: data, "m.mentions": {} }))
                .with_transaction_id(txn.as_str().into())
                .await
                .map_err(err)?;
            Ok(json!({ "event_id": sent.response.event_id.to_string() }))
        }
        "card_edit" => {
            let root = root_id(payload)?;
            let root_ev = root_event(room, &root).await?;
            let kind = text(&root_ev["content"], "msgtype");
            let data = &payload["data"];
            let body = text(payload, "body");
            if text(&root_ev, "sender") != own.as_str() {
                return Err("forbidden".into());
            }
            if card_kind(kind).is_none() || !valid(kind, data) || body.trim().is_empty()
                || body.chars().count() > MAX_FALLBACK
            {
                return Err("invalid".into());
            }
            room.send_raw("m.room.message", json!({ "msgtype": kind, "body": format!("* {body}"),
                    "m.new_content": { "msgtype": kind, "body": body, kind: data, "m.mentions": {} },
                    "m.relates_to": { "rel_type": "m.replace", "event_id": root.as_str() } }))
                .await
                .map_err(err)?;
            Ok(json!({}))
        }
        "card_update" => {
            let root = root_id(payload)?;
            let state = load(room, own, &root, false).await?;
            let op = text(payload, "op");
            let content = match op {
                "add" => {
                    let t = text(payload, "text");
                    if t.trim().is_empty() || t.chars().count() > MAX_ITEM_TEXT {
                        return Err("invalid".into());
                    }
                    json!({ "op": op, "text": t })
                }
                "done" | "undone" | "remove" => {
                    let item = text(payload, "item");
                    if !state["items"].as_array().is_some_and(|a| a.iter().any(|i| i["id"] == item)) {
                        return Err("stale".into());
                    }
                    json!({ "op": op, "item": item })
                }
                _ => return Err("invalid".into()),
            };
            if state["kind"] != "checklist" || state["deleted"] == true || state["can_update"] != true {
                return Err("forbidden".into());
            }
            let mut content = content;
            content["m.relates_to"] = reference(&root);
            room.send_raw(CHECKLIST_UPDATE, content).await.map_err(err)?;
            Ok(json!({}))
        }
        "card_rsvp" => {
            let root = root_id(payload)?;
            let status = text(payload, "status");
            if !["yes", "no", "maybe"].contains(&status) {
                return Err("invalid".into());
            }
            let state = load(room, own, &root, false).await?;
            if state["kind"] != "event" || state["deleted"] == true || state["cancelled"] == true
                || state["can_rsvp"] != true
            {
                return Err("forbidden".into());
            }
            room.send_raw(RSVP, json!({ "status": status, "m.relates_to": reference(&root) }))
                .await
                .map_err(err)?;
            Ok(json!({}))
        }
        _ => Err("invalid".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn root(kind: &str, data: Value) -> Value {
        json!({ "type": "m.room.message", "event_id": "$root", "sender": "@owner:x.org", "origin_server_ts": 1,
            "content": { "msgtype": kind, "body": "fallback", kind: data } })
    }

    fn rel(ty: &str, id: &str, sender: &str, ts: u64, content: Value) -> Value {
        let mut content = content;
        content["m.relates_to"] = json!({ "rel_type": "m.reference", "event_id": "$root" });
        json!({ "type": ty, "event_id": id, "sender": sender, "origin_server_ts": ts, "content": content })
    }

    fn list(anyone: bool) -> Value {
        root(CHECKLIST, json!({ "title": "Groceries", "anyone_edits": anyone,
            "items": [{ "id": "a", "text": "Milk" }, { "id": "b", "text": "Eggs" }] }))
    }

    fn done(state: &Value, id: &str) -> bool {
        state["items"].as_array().unwrap().iter().find(|i| i["id"] == id).unwrap()["done"] == true
    }

    #[test]
    fn concurrent_ticks_and_adds_converge_in_any_arrival_order() {
        let updates = vec![
            rel(CHECKLIST_UPDATE, "$1", "@a:x.org", 10, json!({ "op": "done", "item": "a" })),
            rel(CHECKLIST_UPDATE, "$2", "@b:x.org", 10, json!({ "op": "undone", "item": "a" })),
            rel(CHECKLIST_UPDATE, "$3", "@a:x.org", 11, json!({ "op": "add", "text": "Bread" })),
            rel(CHECKLIST_UPDATE, "$4", "@b:x.org", 11, json!({ "op": "add", "text": "Jam" })),
            rel(CHECKLIST_UPDATE, "$5", "@b:x.org", 12, json!({ "op": "done", "item": "$3" })),
        ];
        let forward = checklist_state(&list(true), &updates, &|_| false);
        let mut reversed = updates.clone();
        reversed.reverse();
        let backward = checklist_state(&list(true), &reversed, &|_| false);
        assert_eq!(forward, backward);
        // Equal timestamps break on event id: "$2" (undone) is applied after "$1".
        assert!(!done(&forward, "a"));
        assert!(done(&forward, "$3"));
        assert_eq!(forward["items"].as_array().unwrap().len(), 4);
        assert_eq!(forward["done_count"], 1);
    }

    #[test]
    fn a_redacted_update_no_longer_counts() {
        let tick = rel(CHECKLIST_UPDATE, "$1", "@a:x.org", 10, json!({ "op": "done", "item": "a" }));
        assert!(done(&checklist_state(&list(true), &[tick], &|_| false), "a"));
        let redacted = json!({ "type": CHECKLIST_UPDATE, "event_id": "$1", "sender": "@a:x.org",
            "origin_server_ts": 10, "content": {} });
        assert!(!done(&checklist_state(&list(true), &[redacted], &|_| false), "a"));
        let mut gone = list(true);
        gone["content"] = json!({});
        assert_eq!(checklist_state(&gone, &[], &|_| false)["deleted"], true);
    }

    #[test]
    fn a_creator_only_list_ignores_other_members_but_not_moderators() {
        let updates = vec![
            rel(CHECKLIST_UPDATE, "$1", "@a:x.org", 10, json!({ "op": "done", "item": "a" })),
            rel(CHECKLIST_UPDATE, "$2", "@mod:x.org", 11, json!({ "op": "done", "item": "b" })),
            rel(CHECKLIST_UPDATE, "$3", "@a:x.org", 12, json!({ "op": "add", "text": "Spam" })),
        ];
        let state = checklist_state(&list(false), &updates, &|u| u == "@mod:x.org");
        assert!(!done(&state, "a"));
        assert!(done(&state, "b"));
        assert_eq!(state["items"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn only_the_creators_valid_edit_replaces_the_card() {
        let edit = |id: &str, sender: &str, title: &str| json!({ "type": "m.room.message", "event_id": id,
            "sender": sender, "origin_server_ts": 5,
            "content": { "m.relates_to": { "rel_type": "m.replace", "event_id": "$root" },
                "m.new_content": { "msgtype": CHECKLIST, "body": "x",
                    CHECKLIST: { "title": title, "items": [{ "id": "a", "text": "Milk" }] } } } });
        let state = checklist_state(&list(true), &[edit("$e1", "@other:x.org", "Hijacked")], &|_| false);
        assert_eq!(state["title"], "Groceries");
        let state = checklist_state(&list(true), &[edit("$e2", "@owner:x.org", "Shopping")], &|_| false);
        assert_eq!(state["title"], "Shopping");
        assert_eq!(state["items"].as_array().unwrap().len(), 1);
    }

    #[test]
    fn hostile_checklist_content_is_refused_or_clamped() {
        let many: Vec<Value> = (0..150).map(|i| json!({ "id": format!("i{i}"), "text": "x" })).collect();
        assert!(!valid_checklist(&json!({ "title": "t", "items": many })));
        assert!(!valid_checklist(&json!({ "title": "t", "items": [{ "id": "a", "text": "x" }, { "id": "a", "text": "y" }] })));
        assert!(!valid_checklist(&json!({ "title": "t", "items": [{ "id": "a", "text": 5 }] })));
        let long = rel(CHECKLIST_UPDATE, "$1", "@a:x.org", 10, json!({ "op": "add", "text": "x".repeat(MAX_ITEM_TEXT + 1) }));
        assert_eq!(checklist_state(&list(true), &[long], &|_| false)["items"].as_array().unwrap().len(), 2);
    }

    fn event(data: Value) -> Value {
        root(EVENT, data)
    }

    #[test]
    fn the_latest_rsvp_per_member_wins_and_an_edit_moves_the_time() {
        let ev = event(json!({ "title": "Meetup", "start": 1_700_000_000_000i64, "end": 0, "tz": "Europe/Vilnius" }));
        let rsvps = vec![
            rel(RSVP, "$1", "@a:x.org", 10, json!({ "status": "yes" })),
            rel(RSVP, "$2", "@a:x.org", 11, json!({ "status": "no" })),
            rel(RSVP, "$3", "@b:x.org", 12, json!({ "status": "yes" })),
            rel(RSVP, "$4", "@c:x.org", 13, json!({ "status": "bogus" })),
            json!({ "type": "m.room.message", "event_id": "$5", "sender": "@owner:x.org", "origin_server_ts": 14,
                "content": { "m.relates_to": { "rel_type": "m.replace", "event_id": "$root" },
                    "m.new_content": { "msgtype": EVENT, "body": "x", EVENT: { "title": "Meetup",
                        "start": 1_700_003_600_000i64, "end": 0, "tz": "Europe/Vilnius", "cancelled": true } } } }),
        ];
        let state = event_state(&ev, &rsvps, "@a:x.org");
        assert_eq!(state["yes"], 1);
        assert_eq!(state["no"], 1);
        assert_eq!(state["own"], "no");
        assert_eq!(state["start"], 1_700_003_600_000i64);
        assert_eq!(state["cancelled"], true);
        assert_eq!(state["going"], json!(["@b:x.org"]));
    }

    #[test]
    fn an_invalid_event_is_not_a_card() {
        assert!(!valid_event(&json!({ "title": "x", "start": 0 })));
        assert!(!valid_event(&json!({ "title": "x", "start": 10, "end": 5 })));
        assert!(!valid_event(&json!({ "title": "x", "start": 10, "end": 0, "link": "http://x.org" })));
        assert!(valid_event(&json!({ "title": "x", "start": 10, "end": 0, "link": "https://x.org/call" })));
        assert_eq!(event_state(&event(json!({ "title": "", "start": 10 })), &[], "")["deleted"], true);
    }
}
