use std::{collections::BTreeMap, sync::Arc, time::Duration};

use matrix_sdk::{config::RequestConfig, ruma::{api::client::state::get_state_events, RoomId}};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::{enqueue, rooms::{classify_room_error, joined_room, require_client}, RustClient};

const GUIDE: &str = "org.lightning_matrix.room_guide";
const PROPOSAL: &str = "org.lightning_matrix.announcement_proposal";
const REVIEW: &str = "org.lightning_matrix.announcement_review";
const LIMIT: usize = 100;

type Result<T> = std::result::Result<T, String>;

fn text<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(Value::as_str).unwrap_or("")
}

fn bounded_text(v: &Value, key: &str, limit: usize) -> bool {
    v.get(key).is_some_and(|s| s.as_str().is_some_and(|s| s.chars().count() <= limit))
}

fn valid_guide(v: &Value) -> bool {
    bounded_text(v, "welcome", 4000) && bounded_text(v, "rules", 4000)
        && v.get("rooms").and_then(Value::as_array).is_some_and(|a| {
            a.len() <= 12 && a.iter().all(|id| id.as_str().is_some_and(|s| RoomId::parse(s).is_ok()))
        })
        && v.get("interests").and_then(Value::as_array).is_some_and(|a| {
            a.len() <= 12 && a.iter().all(|s| s.as_str().is_some_and(|s| !s.trim().is_empty() && s.chars().count() <= 80))
        })
        && v.get("links").and_then(Value::as_array).is_some_and(|a| {
            a.len() <= 8 && a.iter().all(|v| v.as_str().is_some_and(|s| {
                s.len() <= 512 && url::Url::parse(s).is_ok_and(|u| {
                    u.scheme() == "https" && u.host_str().is_some()
                        && u.username().is_empty() && u.password().is_none()
                })
            }))
        })
}

fn valid_proposal(v: &Value) -> bool {
    bounded_text(v, "body", 4000) && !text(v, "body").trim().is_empty()
        && RoomId::parse(text(v, "target")).is_ok()
}

struct State {
    events: BTreeMap<(String, String), Value>,
    own: String,
    joined_count: usize,
}

impl State {
    fn event(&self, ty: &str, key: &str) -> Value {
        self.events.get(&(ty.to_owned(), key.to_owned())).cloned().unwrap_or(Value::Null)
    }
    fn content(&self, ty: &str, key: &str) -> Value {
        self.event(ty, key).get("content").cloned().unwrap_or_else(|| json!({}))
    }
    fn level(&self) -> i64 {
        let create = self.event("m.room.create", "");
        let content = &create["content"];
        let version = text(content, "room_version").parse::<u64>().unwrap_or(1);
        if version >= 12 && (text(&create, "sender") == self.own
            || content["additional_creators"].as_array().is_some_and(|a| a.iter().any(|id| id.as_str() == Some(&self.own)))) {
            return 1i64 << 53;
        }
        let power = self.content("m.room.power_levels", "");
        if self.event("m.room.power_levels", "").is_null() && text(&create, "sender") == self.own {
            return 100;
        }
        power["users"][&self.own].as_i64()
            .unwrap_or_else(|| power["users_default"].as_i64().unwrap_or(0))
    }
    fn can_state(&self, ty: &str) -> bool {
        let p = self.content("m.room.power_levels", "");
        self.joined() && self.level() >= p["events"][ty].as_i64()
            .unwrap_or_else(|| p["state_default"].as_i64().unwrap_or(50))
    }
    fn joined(&self) -> bool {
        text(&self.content("m.room.member", &self.own), "membership") == "join"
    }
    fn can_send(&self) -> bool {
        let p = self.content("m.room.power_levels", "");
        self.joined() && self.level() >= p["events"]["m.room.message"].as_i64()
            .unwrap_or_else(|| p["events_default"].as_i64().unwrap_or(0))
    }
    fn audience(&self, room_id: &str) -> Value {
        json!({ "room_id": room_id,
            "name": text(&self.content("m.room.name", ""), "name"),
            "join_rules": self.content("m.room.join_rules", ""),
            "history": self.content("m.room.history_visibility", ""),
            "joined_count": self.joined_count })
    }
    fn proposal(&self, key: &str) -> Result<Value> {
        let event = self.event(PROPOSAL, key);
        if text(&event, "sender") != key || !valid_proposal(&event["content"])
            || text(&event, "event_id").is_empty() {
            return Err("unavailable".into());
        }
        Ok(event)
    }
    fn snapshot(&self) -> Value {
        let guide = self.content(GUIDE, "");
        let mut proposals = Vec::new();
        let mut truncated = false;
        for (ty, key) in self.events.keys() {
            if ty != PROPOSAL { continue; }
            let Ok(event) = self.proposal(key) else { continue; };
            if proposals.len() == LIMIT { truncated = true; break; }
            let review = self.content(REVIEW, key);
            proposals.push(json!({ "id": key, "author": text(&event, "sender"),
                "revision": text(&event, "event_id"), "body": text(&event["content"], "body"),
                "target": text(&event["content"], "target"),
                "published": text(&review, "revision") == text(&event, "event_id")
                    && text(&review, "status") == "published",
                "publishing": text(&review, "revision") == text(&event, "event_id")
                    && text(&review, "status") == "publishing" }));
        }
        json!({ "guide": if valid_guide(&guide) { guide } else { json!({}) },
            "can_edit_guide": self.can_state(GUIDE), "can_review": self.can_state(REVIEW),
            "can_propose": self.can_state(PROPOSAL), "own_user": self.own,
            "can_enable": self.can_state("m.room.power_levels"),
            "proposals": proposals, "truncated": truncated })
    }
}

async fn fetch(client: &matrix_sdk::Client, room_id: &str) -> Result<State> {
    let room = joined_room(client, room_id)?;
    let own = client.user_id().ok_or("unavailable")?.to_string();
    let config = RequestConfig::new().disable_retry().timeout(Duration::from_secs(20));
    let response = client.send(get_state_events::v3::Request::new(room.room_id().to_owned()))
        .with_request_config(config).await
        .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
    let mut events = BTreeMap::new();
    let mut joined_count = 0;
    for raw in response.room_state {
        let Ok(event) = serde_json::from_str::<Value>(raw.json().get()) else { continue; };
        let ty = text(&event, "type");
        let key = text(&event, "state_key");
        if ty == "m.room.member" && text(&event["content"], "membership") == "join" {
            joined_count += 1;
        }
        if [GUIDE, PROPOSAL, REVIEW, "m.room.power_levels", "m.room.create", "m.room.join_rules",
            "m.room.history_visibility", "m.room.name"].contains(&ty)
            || (ty == "m.room.member" && key == own) {
            events.insert((ty.to_owned(), key.to_owned()), event);
        }
    }
    let state = State { events, own, joined_count };
    if !state.joined() { return Err("forbidden".into()); }
    Ok(state)
}

fn token(proposal: &Value, body: &str, audience: &Value, reviewer: &str) -> String {
    let bytes = serde_json::to_vec(&json!([proposal, body, audience, reviewer])).unwrap_or_default();
    format!("{:x}", Sha256::digest(bytes))
}

fn review_matches(marker: &Value, revision: &str, body: &str, reviewer: &str) -> bool {
    text(marker, "revision") == revision && text(marker, "body") == body
        && text(marker, "reviewer") == reviewer
}

async fn run(client: &matrix_sdk::Client, room_id: &str, action: &str, payload: &Value) -> Result<Value> {
    let state = fetch(client, room_id).await?;
    let room = joined_room(client, room_id)?;
    match action {
        "load" => return Ok(state.snapshot()),
        "guide" => {
            if !state.can_state(GUIDE) { return Err("forbidden".into()); }
            if !valid_guide(payload) { return Err("invalid".into()); }
            room.send_state_event_raw(GUIDE, "", payload.clone()).await
                .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
        }
        "enable" => {
            if !state.can_state("m.room.power_levels") || !state.can_state(REVIEW) {
                return Err("forbidden".into());
            }
            let mut power = state.content("m.room.power_levels", "");
            if !power.is_object() { return Err("unavailable".into()); }
            if !power["events"].is_object() { power["events"] = json!({}); }
            power["events"][PROPOSAL] = json!(0);
            room.send_state_event_raw("m.room.power_levels", "", power).await
                .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
        }
        "propose" => {
            if !state.can_state(PROPOSAL) { return Err("forbidden".into()); }
            if !valid_proposal(payload) { return Err("invalid".into()); }
            let target = fetch(client, text(payload, "target")).await?;
            if !target.joined() { return Err("forbidden".into()); }
            let existing = state.event(PROPOSAL, &state.own);
            let marker = state.content(REVIEW, &state.own);
            if text(&marker, "revision") == text(&existing, "event_id")
                && text(&marker, "status") == "publishing" {
                return Err("pending".into());
            }
            room.send_state_event_raw(PROPOSAL, &state.own,
                json!({ "body": text(payload, "body"), "target": text(payload, "target") })).await
                .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
        }
        "preview" | "publish" => {
            if !state.can_state(REVIEW) || !bounded_text(payload, "body", 4000)
                || text(payload, "body").trim().is_empty() {
                return Err("forbidden".into());
            }
            let id = text(payload, "id");
            let proposal = state.proposal(id)?;
            let target_id = text(&proposal["content"], "target");
            let target = fetch(client, target_id).await?;
            if !target.can_send() { return Err("forbidden".into()); }
            let audience = target.audience(target_id);
            let body = text(payload, "body");
            let approval = token(&proposal, body, &audience, &state.own);
            if action == "preview" {
                return Ok(json!({ "approval": approval, "audience": audience,
                    "body": body, "author": text(&proposal, "sender"), "publisher": state.own,
                    "revision": text(&proposal, "event_id") }));
            }
            let marker = state.content(REVIEW, id);
            let revision = text(&proposal, "event_id");
            let same = review_matches(&marker, revision, body, &state.own);
            if text(&marker, "revision") == revision && text(&marker, "status") == "published" {
                return Ok(json!({ "published": true }));
            }
            // Retries keep the reviewed text and transaction even if the member count changed.
            if !(same && text(&marker, "status") == "publishing")
                && text(payload, "approval") != approval {
                return Err("stale".into());
            }
            if text(&marker, "revision") == revision && text(&marker, "status") == "publishing" && !same {
                return Err("pending".into());
            }
            let txn = if same && text(&marker, "status") == "publishing" {
                text(&marker, "transaction").to_owned()
            } else {
                format!("lightning-announcement-{}", token(&proposal, body, &json!(target_id), &state.own))
            };
            if txn.is_empty() { return Err("unavailable".into()); }
            let mut audit = json!({ "status": "publishing", "revision": revision,
                "body": body, "target": target_id, "reviewer": state.own,
                "author": text(&proposal, "sender"), "transaction": txn });
            room.send_state_event_raw(REVIEW, id, audit.clone()).await
                .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
            let destination = joined_room(client, target_id)?;
            let sent = destination.send_raw("m.room.message", json!({ "msgtype": "m.text", "body": body,
                "org.lightning_matrix.announcement": { "space": room_id, "proposal": id,
                    "revision": revision, "author": text(&proposal, "sender"), "reviewer": state.own },
                "m.mentions": {} }))
                .with_transaction_id(txn.as_str().into()).await
                .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
            audit["status"] = json!("published");
            audit["event_id"] = json!(sent.event_id.to_string());
            room.send_state_event_raw(REVIEW, id, audit).await
                .map_err(|e| classify_room_error(&e.to_string()).to_owned())?;
            return Ok(json!({ "published": true }));
        }
        _ => return Err("invalid".into()),
    }
    Ok(json!({}))
}

pub(crate) fn dispatch(bridge: &RustClient, room_id: String, action: String, payload: String, op_id: u64) -> Result<()> {
    if payload.len() > 48 * 1024 || !["load", "guide", "enable", "propose", "preview", "publish"].contains(&action.as_str()) {
        return Err("invalid".into());
    }
    let payload: Value = serde_json::from_str(&payload).map_err(|_| "invalid".to_owned())?;
    let client = require_client(bridge)?;
    joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = tokio::time::timeout(Duration::from_secs(100), run(&client, &room_id, &action, &payload)).await
            .unwrap_or_else(|_| Err("unknown".into()));
        if !timelines.lifecycle_current(lifecycle) { return; }
        let (ok, result) = match result {
            Ok(v) => (true, v), Err(category) => (false, json!({ "category": category })),
        };
        enqueue(&events, json!({ "type": "space_community_result", "op_id": op_id,
            "lifecycle": lifecycle, "room_id": room_id, "action": action, "ok": ok, "result": result }));
    });
    Ok(())
}
