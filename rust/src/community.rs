use std::{collections::BTreeMap, sync::Arc, time::Duration};

use matrix_sdk::{
    config::RequestConfig,
    deserialized_responses::RawAnySyncOrStrippedState,
    ruma::{
        api::client::state::{get_state_event_for_key, get_state_events},
        events::{
            room::power_levels::{RoomPowerLevels, RoomPowerLevelsEventContent},
            MessageLikeEventType, StateEventType,
            TimelineEventType,
        },
        room::{AllowRule, JoinRule},
        Int, RoomId, UserId,
    },
    Client, Room,
};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::{
    enqueue, optprobe,
    rooms::{classify_room_error, join_rule_str, joined_room, require_client},
    RustClient,
};

const GUIDE: &str = "org.lightning_matrix.room_guide";
const PROPOSAL: &str = "org.lightning_matrix.announcement_proposal";
const REVIEW: &str = "org.lightning_matrix.announcement_review";
const LIMIT: usize = 100;
const MAX_BODY: usize = 4000;

type Result<T> = std::result::Result<T, String>;

pub(crate) fn err(e: impl ToString) -> String {
    classify_room_error(&e.to_string()).to_owned()
}

pub(crate) fn text<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(Value::as_str).unwrap_or("")
}

fn bounded_text(v: &Value, key: &str, limit: usize) -> bool {
    v.get(key).and_then(Value::as_str).is_some_and(|s| s.chars().count() <= limit)
}

fn valid_guide(v: &Value) -> bool {
    bounded_text(v, "welcome", MAX_BODY)
        && bounded_text(v, "rules", MAX_BODY)
        && v.get("rooms").and_then(Value::as_array).is_some_and(|a| {
            a.len() <= 12 && a.iter().all(|id| id.as_str().is_some_and(|s| RoomId::parse(s).is_ok()))
        })
        && v.get("links").and_then(Value::as_array).is_some_and(|a| {
            a.len() <= 8
                && a.iter().all(|v| v.as_str().is_some_and(https_link))
        })
}

pub(crate) fn https_link(s: &str) -> bool {
    s.len() <= 512
        && url::Url::parse(s).is_ok_and(|u| {
            u.scheme() == "https" && u.host_str().is_some() && u.username().is_empty() && u.password().is_none()
        })
}

fn clamp(v: &Value, key: &str) -> String {
    text(v, key).chars().take(MAX_BODY).collect()
}

/// The guide is written by other members: keep only what passes the write rules, cut to the limits.
fn read_guide(v: &Value) -> Value {
    let strings = |key: &str, keep: &dyn Fn(&str) -> bool, max: usize| -> Vec<String> {
        v.get(key)
            .and_then(Value::as_array)
            .map(|a| a.iter().filter_map(Value::as_str).filter(|s| keep(s)).take(max).map(str::to_owned).collect())
            .unwrap_or_default()
    };
    json!({ "welcome": clamp(v, "welcome"), "rules": clamp(v, "rules"),
        "rooms": strings("rooms", &|s| RoomId::parse(s).is_ok(), 12),
        "links": strings("links", &https_link, 8) })
}

fn author_may_propose(levels: &RoomPowerLevels, author: &str) -> bool {
    UserId::parse(author).is_ok_and(|u| levels.user_can_send_state(&u, StateEventType::from(PROPOSAL)))
}

/// The proposal a reviewer may act on: present, well formed, and its author may still propose.
fn publishable<'a>(state: &'a StateMap, author: &str, levels: &RoomPowerLevels) -> Result<&'a Value> {
    let event = proposal(state, author).ok_or("stale")?;
    if !author_may_propose(levels, author) {
        return Err("stale".into());
    }
    Ok(event)
}

fn valid_body(v: &Value) -> bool {
    bounded_text(v, "body", MAX_BODY) && !text(v, "body").trim().is_empty()
}

fn valid_proposal(v: &Value) -> bool {
    valid_body(v) && RoomId::parse(text(v, "target")).is_ok()
}

type StateMap = BTreeMap<(String, String), Value>;

fn index(events: impl IntoIterator<Item = Value>) -> StateMap {
    events
        .into_iter()
        .filter(|e| [PROPOSAL, REVIEW].contains(&text(e, "type")))
        .map(|e| ((text(&e, "type").to_owned(), text(&e, "state_key").to_owned()), e))
        .collect()
}

/// A proposal counts only when its state key is its sender, so nobody can file one for another user.
fn proposal<'a>(state: &'a StateMap, author: &str) -> Option<&'a Value> {
    state.get(&(PROPOSAL.to_owned(), author.to_owned())).filter(|e| {
        text(e, "sender") == author
            && !text(e, "event_id").is_empty()
            && valid_proposal(&e["content"])
    })
}

/// Review markers are keyed by the proposal's event id: an `@` state key must equal its sender.
fn marker(state: &StateMap, revision: &str) -> Value {
    state
        .get(&(REVIEW.to_owned(), revision.to_owned()))
        .and_then(|e| e.get("content").cloned())
        .unwrap_or_else(|| json!({}))
}

fn proposals(state: &StateMap, authorized: &dyn Fn(&str) -> bool) -> (Vec<Value>, bool) {
    let mut out = Vec::new();
    for (ty, key) in state.keys() {
        if ty != PROPOSAL {
            continue;
        }
        let Some(event) = proposal(state, key) else { continue };
        if out.len() == LIMIT {
            return (out, true);
        }
        let revision = text(event, "event_id");
        let status = text(&marker(state, revision), "status").to_owned();
        out.push(json!({ "id": key, "author": key, "revision": revision,
            "body": text(&event["content"], "body"), "target": text(&event["content"], "target"),
            "published": status == "published", "publishing": status == "publishing",
            "authorized": authorized(key) }));
    }
    (out, false)
}

pub(crate) fn digest(value: &Value) -> String {
    format!("{:x}", Sha256::digest(serde_json::to_vec(value).unwrap_or_default()))
}

fn approval_token(revision: &str, body: &str, audience: &Value, reviewer: &str) -> String {
    digest(&json!([revision, body, audience["room_id"], audience["join_rule"],
        audience["allow"], audience["history"], reviewer]))
}

fn transaction(revision: &str, body: &str, target: &str, reviewer: &str) -> String {
    format!("lightning-announcement-{}", digest(&json!([revision, body, target, reviewer])))
}

fn config() -> RequestConfig {
    RequestConfig::new().disable_retry().timeout(Duration::from_secs(20))
}

async fn space_state(client: &Client, room: &Room) -> Result<StateMap> {
    let response = client
        .send(get_state_events::v3::Request::new(room.room_id().to_owned()))
        .with_request_config(config())
        .await
        .map_err(err)?;
    Ok(index(
        response
            .room_state
            .iter()
            .filter_map(|raw| serde_json::from_str::<Value>(raw.json().get()).ok()),
    ))
}

async fn guide(client: &Client, room: &Room) -> Value {
    if let Ok(Some(raw)) = room.get_state_event(StateEventType::from(GUIDE), "").await {
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
            RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
        };
        if let Ok(v) = serde_json::from_str::<Value>(&json) {
            return v["content"].clone();
        }
    }
    let room_key = room.room_id().to_string();
    if optprobe::state_known_absent(client, &room_key, GUIDE) {
        return json!({});
    }
    let request = get_state_event_for_key::v3::Request::new(
        room.room_id().to_owned(),
        StateEventType::from(GUIDE),
        String::new(),
    );
    optprobe::state_pace(client).await;
    match client.send(request).with_request_config(config()).await {
        Ok(response) => {
            serde_json::from_str(response.event_or_content.get()).unwrap_or_else(|_| json!({}))
        }
        Err(e) => {
            if e.as_client_api_error().is_some_and(|e| e.status_code.as_u16() == 404) {
                optprobe::state_mark_absent(client, &room_key, GUIDE);
            }
            json!({})
        }
    }
}

struct Perms {
    guide: bool,
    propose: bool,
    review: bool,
    enable: bool,
    open: bool,
}

async fn perms(room: &Room, own: &UserId) -> Perms {
    let Ok(levels) = room.power_levels().await else {
        return Perms { guide: false, propose: false, review: false, enable: false, open: false };
    };
    let can = |ty: &str| levels.user_can_send_state(own, StateEventType::from(ty));
    Perms {
        guide: can(GUIDE),
        propose: can(PROPOSAL),
        review: can(REVIEW),
        enable: can("m.room.power_levels") && can(REVIEW),
        open: levels
            .events
            .get(&TimelineEventType::from(PROPOSAL))
            .is_some_and(|level| *level <= levels.users_default),
    }
}

fn audience(room: &Room) -> Value {
    let allow: Vec<String> = match room.join_rule() {
        Some(JoinRule::Restricted(r)) | Some(JoinRule::KnockRestricted(r)) => r
            .allow
            .iter()
            .filter_map(|rule| match rule {
                AllowRule::RoomMembership(m) => Some(m.room_id.to_string()),
                _ => None,
            })
            .collect(),
        _ => Vec::new(),
    };
    json!({ "room_id": room.room_id().to_string(),
        "name": room.cached_display_name().map(|n| n.to_string()).unwrap_or_default(),
        "join_rule": join_rule_str(room.join_rule().as_ref()), "allow": allow,
        "history": room.history_visibility().map(|h| h.as_str().to_owned()).unwrap_or_default(),
        "joined_count": room.joined_members_count() })
}

async fn load(client: &Client, room: &Room, own: &UserId) -> Value {
    let g = guide(client, room).await;
    let p = perms(room, own).await;
    json!({ "guide": read_guide(&g),
        "can_edit_guide": p.guide, "can_propose": p.propose, "can_review": p.review,
        "can_enable": p.enable, "proposals_open": p.open, "own_user": own.to_string() })
}

async fn run(client: &Client, room_id: &str, action: &str, payload: &Value) -> Result<Value> {
    let room = joined_room(client, room_id)?;
    let own = client.user_id().ok_or("unavailable")?.to_owned();
    match action {
        "load" => Ok(load(client, &room, &own).await),
        "proposals" => {
            let state = space_state(client, &room).await?;
            let mut out = load(client, &room, &own).await;
            let levels = room.power_levels().await.ok();
            let (list, truncated) =
                proposals(&state, &|a| levels.as_ref().is_some_and(|l| author_may_propose(l, a)));
            out["proposals"] = json!(list);
            out["truncated"] = json!(truncated);
            Ok(out)
        }
        "guide" => {
            if !perms(&room, &own).await.guide {
                return Err("forbidden".into());
            }
            if !valid_guide(payload) {
                return Err("invalid".into());
            }
            let content = json!({ "welcome": text(payload, "welcome"), "rules": text(payload, "rules"),
                "rooms": payload["rooms"], "links": payload["links"] });
            room.send_state_event_raw(GUIDE, "", content).await.map_err(err)?;
            optprobe::state_forget(client, room_id, GUIDE);
            Ok(json!({}))
        }
        "enable" => {
            if !perms(&room, &own).await.enable {
                return Err("forbidden".into());
            }
            let levels = room.power_levels().await.map_err(err)?;
            let mut content = RoomPowerLevelsEventContent::try_from(levels).map_err(err)?;
            content.events.insert(TimelineEventType::from(PROPOSAL), Int::from(0u8));
            room.send_state_event(content).await.map_err(err)?;
            Ok(json!({}))
        }
        "propose" => {
            if !perms(&room, &own).await.propose {
                return Err("forbidden".into());
            }
            if !valid_proposal(payload) {
                return Err("invalid".into());
            }
            joined_room(client, text(payload, "target")).map_err(|_| "forbidden".to_owned())?;
            room.send_state_event_raw(
                PROPOSAL,
                own.as_str(),
                json!({ "body": text(payload, "body"), "target": text(payload, "target") }),
            )
            .await
            .map_err(err)?;
            Ok(json!({}))
        }
        "preview" | "publish" => {
            if !perms(&room, &own).await.review || !valid_body(payload) {
                return Err("forbidden".into());
            }
            let state = space_state(client, &room).await?;
            let author = text(payload, "id");
            let levels = room.power_levels().await.map_err(err)?;
            let event = publishable(&state, author, &levels)?;
            let revision = text(event, "event_id");
            let target_id = text(&event["content"], "target");
            let target = joined_room(client, target_id).map_err(|_| "forbidden".to_owned())?;
            let can_send = target
                .power_levels()
                .await
                .is_ok_and(|l| l.user_can_send_message(&own, MessageLikeEventType::RoomMessage));
            if !can_send {
                return Err("forbidden".into());
            }
            let audience = audience(&target);
            let body = text(payload, "body");
            let approval = approval_token(revision, body, &audience, own.as_str());
            if action == "preview" {
                return Ok(json!({ "approval": approval, "audience": audience, "body": body,
                    "author": author, "publisher": own.to_string(), "revision": revision }));
            }
            let mark = marker(&state, revision);
            let status = text(&mark, "status");
            if status == "published" {
                return Ok(json!({ "published": true }));
            }
            // A retry of this reviewer's own interrupted publish reuses the same transaction id.
            let resuming = status == "publishing"
                && text(&mark, "body") == body
                && text(&mark, "reviewer") == own.as_str();
            if status == "publishing" && !resuming {
                return Err("pending".into());
            }
            if !resuming && text(payload, "approval") != approval {
                return Err("stale".into());
            }
            let txn = transaction(revision, body, target_id, own.as_str());
            let mut audit = json!({ "status": "publishing", "revision": revision, "body": body,
                "target": target_id, "reviewer": own.to_string(), "author": author });
            room.send_state_event_raw(REVIEW, revision, audit.clone()).await.map_err(err)?;
            let sent = target
                .send_raw("m.room.message", json!({ "msgtype": "m.text", "body": body,
                    "org.lightning_matrix.announcement": { "space": room_id,
                        "revision": revision, "author": author, "reviewer": own.to_string() },
                    "m.mentions": {} }))
                .with_transaction_id(txn.as_str().into())
                .await
                .map_err(err)?;
            audit["status"] = json!("published");
            audit["event_id"] = json!(sent.response.event_id.to_string());
            room.send_state_event_raw(REVIEW, revision, audit).await.map_err(err)?;
            Ok(json!({ "published": true }))
        }
        a if a.starts_with("card_") => crate::roomcards::run(&room, &own, a, payload).await,
        a if a.starts_with("mod_") => crate::modqueue::run(client, &room, &own, a, payload).await,
        _ => Err("invalid".into()),
    }
}

const ACTIONS: [&str; 14] = ["load", "proposals", "guide", "enable", "propose", "preview", "publish",
    "card_load", "card_create", "card_edit", "card_update", "card_rsvp", "mod_cases", "mod_act"];

pub(crate) fn dispatch(
    bridge: &RustClient,
    room_id: String,
    action: String,
    payload: String,
    op_id: u64,
) -> Result<()> {
    if payload.len() > 48 * 1024 || !ACTIONS.contains(&action.as_str()) {
        return Err("invalid".into());
    }
    let payload: Value = serde_json::from_str(&payload).map_err(|_| "invalid".to_owned())?;
    let client = require_client(bridge)?;
    joined_room(&client, &room_id)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result =
            tokio::time::timeout(Duration::from_secs(100), run(&client, &room_id, &action, &payload))
                .await
                .unwrap_or_else(|_| Err("unknown".into()));
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, result) = match result {
            Ok(v) => (true, v),
            Err(category) => (false, json!({ "category": category })),
        };
        enqueue(&events, json!({ "type": "space_community_result", "op_id": op_id,
            "lifecycle": lifecycle, "room_id": room_id, "action": action, "ok": ok, "result": result }));
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(ty: &str, key: &str, sender: &str, id: &str, content: Value) -> Value {
        json!({ "type": ty, "state_key": key, "sender": sender, "event_id": id, "content": content,
            "unsigned": { "age": 1 } })
    }

    fn proposal_ev(author: &str, id: &str) -> Value {
        ev(PROPOSAL, author, author, id, json!({ "body": "Hello", "target": "!t:x.org" }))
    }

    #[test]
    fn a_proposal_filed_under_another_users_key_is_ignored() {
        let state = index([
            proposal_ev("@a:x.org", "$1"),
            ev(PROPOSAL, "@b:x.org", "@a:x.org", "$2", json!({ "body": "Hi", "target": "!t:x.org" })),
        ]);
        let (list, truncated) = proposals(&state, &|_| true);
        assert_eq!(list.len(), 1);
        assert_eq!(list[0]["author"], "@a:x.org");
        assert!(!truncated);
    }

    #[test]
    fn a_review_marker_applies_only_to_the_revision_it_names() {
        let old = index([
            proposal_ev("@a:x.org", "$new"),
            ev(REVIEW, "$old", "@mod:x.org", "$r", json!({ "status": "published" })),
        ]);
        assert_eq!(proposals(&old, &|_| true).0[0]["published"], false);
        let current = index([
            proposal_ev("@a:x.org", "$new"),
            ev(REVIEW, "$new", "@mod:x.org", "$r", json!({ "status": "published" })),
        ]);
        assert_eq!(proposals(&current, &|_| true).0[0]["published"], true);
    }

    #[test]
    fn approval_tracks_text_audience_and_revision_but_not_member_count() {
        let audience = json!({ "room_id": "!t:x.org", "join_rule": "invite", "allow": [],
            "history": "shared", "joined_count": 3 });
        let base = approval_token("$1", "Hello", &audience, "@mod:x.org");
        let mut grown = audience.clone();
        grown["joined_count"] = json!(4);
        assert_eq!(base, approval_token("$1", "Hello", &grown, "@mod:x.org"));
        assert_ne!(base, approval_token("$1", "Hello!", &audience, "@mod:x.org"));
        assert_ne!(base, approval_token("$2", "Hello", &audience, "@mod:x.org"));
        let mut public = audience.clone();
        public["join_rule"] = json!("public");
        assert_ne!(base, approval_token("$1", "Hello", &public, "@mod:x.org"));
    }

    #[test]
    fn a_retried_publish_reuses_its_transaction_id() {
        let first = transaction("$1", "Hello", "!t:x.org", "@mod:x.org");
        assert_eq!(first, transaction("$1", "Hello", "!t:x.org", "@mod:x.org"));
        assert_ne!(first, transaction("$2", "Hello", "!t:x.org", "@mod:x.org"));
    }

    fn levels(content: Value) -> RoomPowerLevels {
        use matrix_sdk::ruma::{room_version_rules::AuthorizationRules, OwnedUserId};
        let content: RoomPowerLevelsEventContent = serde_json::from_value(content).unwrap();
        RoomPowerLevels::new(content.into(), &AuthorizationRules::V1, Vec::<OwnedUserId>::new())
    }

    #[test]
    fn a_proposal_whose_author_lost_the_power_to_propose_is_never_publishable() {
        let state = index([proposal_ev("@a:x.org", "$1")]);
        let open = levels(json!({ "events": { "org.lightning_matrix.announcement_proposal": 0 }, "users": {} }));
        assert!(publishable(&state, "@a:x.org", &open).is_ok());
        let lowered = levels(json!({ "events": { "org.lightning_matrix.announcement_proposal": 0 }, "users": { "@a:x.org": -10 } }));
        assert_eq!(publishable(&state, "@a:x.org", &lowered).unwrap_err(), "stale");
        let closed = levels(json!({ "events": { "org.lightning_matrix.announcement_proposal": 50 }, "users": {} }));
        assert!(publishable(&state, "@a:x.org", &closed).is_err());
        let list = proposals(&state, &|a| author_may_propose(&lowered, a)).0;
        assert_eq!(list[0]["authorized"], false);
    }

    #[test]
    fn hostile_guide_state_is_clamped_on_read() {
        let rooms: Vec<Value> = (0..20).map(|i| json!(format!("!r{i}:x.org"))).chain([json!("nope"), json!(3)]).collect();
        let mut links: Vec<Value> = (0..20).map(|i| json!(format!("https://x.org/{i}"))).collect();
        links.splice(0..0, [json!("http://x.org"), json!("javascript:alert(1)"), json!(7),
            json!("https://u:p@x.org"), json!("file:///etc/passwd")]);
        let read = read_guide(&json!({ "welcome": "w".repeat(9000), "rules": ["not", "text"],
            "rooms": rooms, "links": links }));
        assert_eq!(read["welcome"].as_str().unwrap().chars().count(), MAX_BODY);
        assert_eq!(read["rules"], "");
        assert_eq!(read["rooms"].as_array().unwrap().len(), 12);
        let links = read["links"].as_array().unwrap();
        assert_eq!(links.len(), 8);
        assert!(links.iter().all(|l| l.as_str().unwrap().starts_with("https://x.org/")));
        assert_eq!(read_guide(&json!("garbage")), json!({ "welcome": "", "rules": "", "rooms": [], "links": [] }));
    }

    #[test]
    fn hostile_proposal_state_is_ignored_or_capped() {
        let mut events: Vec<Value> =
            (0..150).map(|i| proposal_ev(&format!("@u{i:03}:x.org"), &format!("${i}"))).collect();
        events.push(ev(PROPOSAL, "@long:x.org", "@long:x.org", "$l",
            json!({ "body": "x".repeat(MAX_BODY + 1), "target": "!t:x.org" })));
        events.push(ev(PROPOSAL, "@typed:x.org", "@typed:x.org", "$t", json!({ "body": 5, "target": "!t:x.org" })));
        events.push(ev(PROPOSAL, "@bad:x.org", "@bad:x.org", "$b", json!({ "body": "Hi", "target": "nope" })));
        let state = index(events);
        let (list, truncated) = proposals(&state, &|_| true);
        assert_eq!(list.len(), LIMIT);
        assert!(truncated);
        assert!(list.iter().all(|p| p["author"].as_str().unwrap().starts_with("@u")));
        for author in ["@long:x.org", "@typed:x.org", "@bad:x.org"] {
            assert!(proposal(&state, author).is_none(), "{author}");
        }
    }

    #[test]
    fn guide_links_must_be_plain_https() {
        let ok = json!({ "welcome": "", "rules": "", "rooms": ["!a:x.org"], "links": ["https://x.org/r"] });
        assert!(valid_guide(&ok));
        for bad in ["http://x.org", "javascript:alert(1)", "https://u:p@x.org"] {
            let mut g = ok.clone();
            g["links"] = json!([bad]);
            assert!(!valid_guide(&g), "{bad}");
        }
    }
}
