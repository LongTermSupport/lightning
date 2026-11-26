//! Moderation case queue: room members covered by a followed policy list, one
//! case per member with every matching rule as evidence. Nothing is enforced
//! here: a ban runs only for one confirmed case, after the rule, the target's
//! membership and the viewer's power are all re-checked.

use std::time::Duration;

use matrix_sdk::{
    ruma::{events::GlobalAccountDataEventType, serde::Raw, UserId},
    Client, Room, RoomMemberships,
};
use serde_json::{json, Value};

use crate::{
    community::{err, text},
    policy::{find_match, read_rules, read_subscriptions, Rule},
    rooms::{joined_room, moderation_plan_row},
};

const DECISIONS: &str = "org.lightning_matrix.moderation_cases";
const MAX_CASES: usize = 200;
const MAX_MEMBERS: usize = 20_000;
const MAX_DECISIONS: usize = 500;

type Result<T> = std::result::Result<T, String>;

fn evidence(lists: &[(String, Vec<Rule>)], user: &str) -> Vec<Value> {
    let mut out: Vec<Value> = Vec::new();
    for (list, rules) in lists {
        if let Some(rule) = find_match(rules, "user", user) {
            let item = json!({ "list": list, "kind": rule.kind, "entity": rule.entity, "reason": rule.reason });
            if !out.contains(&item) {
                out.push(item);
            }
        }
    }
    out
}

/// The latest decision recorded for this room and user: "open", "dismissed" or "banned".
fn status(decisions: &[Value], room: &str, user: &str) -> String {
    decisions
        .iter()
        .filter(|d| text(d, "room") == room && text(d, "user") == user)
        .max_by_key(|d| d["ts"].as_u64().unwrap_or(0))
        .map(|d| text(d, "status").to_owned())
        .filter(|s| ["open", "dismissed", "banned"].contains(&s.as_str()))
        .unwrap_or_else(|| "open".to_owned())
}

/// One case per member, however many lists or rules match; the viewer is never a case.
pub(crate) fn group_cases(
    members: &[(String, String)],
    lists: &[(String, Vec<Rule>)],
    decisions: &[Value],
    room: &str,
    own: &str,
) -> (Vec<Value>, bool) {
    let mut cases = Vec::new();
    for (user, membership) in members {
        if user == own || cases.iter().any(|c: &Value| c["user"] == user.as_str()) {
            continue;
        }
        let found = evidence(lists, user);
        if found.is_empty() {
            continue;
        }
        if cases.len() == MAX_CASES {
            return (cases, true);
        }
        cases.push(json!({ "user": user, "membership": membership, "evidence": found,
            "proposed": "ban", "status": status(decisions, room, user) }));
    }
    (cases, false)
}

/// Keeps the newest decision per (room, user), at most `MAX_DECISIONS` in all.
pub(crate) fn record(decisions: &[Value], room: &str, user: &str, status: &str, ts: u64) -> Vec<Value> {
    let mut out: Vec<Value> = decisions
        .iter()
        .filter(|d| !(text(d, "room") == room && text(d, "user") == user))
        .filter(|d| !text(d, "room").is_empty() && !text(d, "user").is_empty())
        .cloned()
        .collect();
    out.push(json!({ "room": room, "user": user, "status": status, "ts": ts }));
    out.sort_by_key(|d| std::cmp::Reverse(d["ts"].as_u64().unwrap_or(0)));
    out.truncate(MAX_DECISIONS);
    out
}

async fn decisions(client: &Client) -> Vec<Value> {
    let ty = GlobalAccountDataEventType::from(DECISIONS);
    let Ok(Some(raw)) = client.account().fetch_account_data(ty).await else {
        return Vec::new();
    };
    serde_json::from_str::<Value>(raw.json().get())
        .ok()
        .and_then(|v| v["decisions"].as_array().cloned())
        .map(|mut d| {
            d.truncate(MAX_DECISIONS);
            d
        })
        .unwrap_or_default()
}

async fn decide(client: &Client, room: &str, user: &str, status: &str) -> Result<()> {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    let next = record(&decisions(client).await, room, user, status, now);
    let raw = Raw::new(&json!({ "decisions": next })).map_err(err)?.cast_unchecked();
    client
        .account()
        .set_account_data_raw(GlobalAccountDataEventType::from(DECISIONS), raw)
        .await
        .map_err(err)?;
    Ok(())
}

async fn lists(client: &Client) -> (Vec<(String, Vec<Rule>)>, bool) {
    let mut out = Vec::new();
    let mut truncated = false;
    for id in read_subscriptions(client).await {
        let Ok(room) = joined_room(client, &id) else { continue };
        let (rules, cut) = read_rules(client, &room).await;
        truncated |= cut;
        out.push((id, rules));
    }
    (out, truncated)
}

pub(crate) async fn run(client: &Client, room: &Room, own: &UserId, action: &str, payload: &Value) -> Result<Value> {
    let room_id = room.room_id().to_string();
    match action {
        "mod_cases" => {
            let (lists, rules_truncated) = lists(client).await;
            let members = tokio::time::timeout(
                Duration::from_secs(30),
                room.members(RoomMemberships::JOIN | RoomMemberships::INVITE | RoomMemberships::KNOCK),
            )
            .await
            .map_err(|_| "unknown".to_owned())?
            .map_err(err)?;
            let scanned: Vec<(String, String)> = members
                .iter()
                .take(MAX_MEMBERS)
                .map(|m| (m.user_id().to_string(), m.membership().as_str().to_owned()))
                .collect();
            let (cases, cut) = group_cases(&scanned, &lists, &decisions(client).await, &room_id, own.as_str());
            let can_ban = room.get_member_no_sync(own).await.ok().flatten().is_some_and(|m| m.can_ban());
            Ok(json!({ "cases": cases, "lists": lists.len(), "can_ban": can_ban,
                "truncated": cut || rules_truncated || members.len() > MAX_MEMBERS }))
        }
        "mod_act" => {
            let user = text(payload, "user");
            let uid = UserId::parse(user).map_err(|_| "invalid".to_owned())?;
            match text(payload, "decision") {
                s @ ("dismissed" | "open") => {
                    decide(client, &room_id, user, s).await?;
                    return Ok(json!({ "status": s }));
                }
                "ban" => {}
                _ => return Err("invalid".into()),
            }
            let (lists, _) = lists(client).await;
            if evidence(&lists, user).is_empty() {
                return Err("stale".into());
            }
            let row = moderation_plan_row(client, room_id.clone(), &uid, own, 1).await;
            let verdict = text(&row, "reason");
            if !verdict.is_empty() {
                return Err(verdict.to_owned());
            }
            let reason = text(payload, "reason").chars().take(500).collect::<String>();
            room.ban_user(&uid, (!reason.is_empty()).then_some(reason.as_str())).await.map_err(err)?;
            // The ban is done; failing to record it only leaves the case open.
            let recorded = decide(client, &room_id, user, "banned").await.is_ok();
            Ok(json!({ "status": "banned", "recorded": recorded }))
        }
        _ => Err("invalid".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rule(kind: &str, entity: &str, reason: &str) -> Rule {
        Rule { kind: kind.into(), entity: entity.into(), recommendation: "m.ban".into(),
            reason: reason.into(), state_key: entity.into() }
    }

    fn members(ids: &[&str]) -> Vec<(String, String)> {
        ids.iter().map(|u| (u.to_string(), "join".to_owned())).collect()
    }

    #[test]
    fn one_case_per_member_with_every_list_as_evidence() {
        let lists = vec![
            ("!a:x.org".to_owned(), vec![rule("user", "@spam*:evil.org", "spam")]),
            ("!b:x.org".to_owned(), vec![rule("server", "evil.org", "spam server"),
                Rule { recommendation: "org.example.warn".into(), ..rule("user", "@ok:x.org", "") }]),
        ];
        let (cases, truncated) = group_cases(
            &members(&["@spammer:evil.org", "@ok:x.org", "@me:evil.org", "@spammer:evil.org"]),
            &lists, &[], "!r:x.org", "@me:evil.org");
        assert!(!truncated);
        assert_eq!(cases.len(), 1);
        assert_eq!(cases[0]["user"], "@spammer:evil.org");
        assert_eq!(cases[0]["evidence"].as_array().unwrap().len(), 2);
        assert_eq!(cases[0]["status"], "open");
    }

    #[test]
    fn a_decision_applies_to_its_own_room_only_and_the_newest_wins() {
        let lists = vec![("!a:x.org".to_owned(), vec![rule("user", "@bad:x.org", "")])];
        let d = record(&[], "!r:x.org", "@bad:x.org", "dismissed", 10);
        let d = record(&d, "!other:x.org", "@bad:x.org", "banned", 11);
        let (cases, _) = group_cases(&members(&["@bad:x.org"]), &lists, &d, "!r:x.org", "@me:x.org");
        assert_eq!(cases[0]["status"], "dismissed");
        let d = record(&d, "!r:x.org", "@bad:x.org", "open", 12);
        assert_eq!(d.len(), 2);
        let (cases, _) = group_cases(&members(&["@bad:x.org"]), &lists, &d, "!r:x.org", "@me:x.org");
        assert_eq!(cases[0]["status"], "open");
    }

    #[test]
    fn decisions_and_cases_are_bounded() {
        let mut d = Vec::new();
        for i in 0..(MAX_DECISIONS + 20) {
            d = record(&d, "!r:x.org", &format!("@u{i}:x.org"), "dismissed", i as u64);
        }
        assert_eq!(d.len(), MAX_DECISIONS);
        assert_eq!(d[0]["user"], format!("@u{}:x.org", MAX_DECISIONS + 19));
        let lists = vec![("!a:x.org".to_owned(), vec![rule("server", "evil.org", "")])];
        let many: Vec<String> = (0..300).map(|i| format!("@u{i}:evil.org")).collect();
        let refs: Vec<&str> = many.iter().map(String::as_str).collect();
        let (cases, truncated) = group_cases(&members(&refs), &lists, &[], "!r:x.org", "@me:x.org");
        assert_eq!(cases.len(), MAX_CASES);
        assert!(truncated);
    }
}
