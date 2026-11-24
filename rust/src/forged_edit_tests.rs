use super::*;
use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw, RoomId};
use matrix_sdk::test_utils::mocks::MatrixMockServer;
use matrix_sdk_base::crypto::{olm::EncryptionSettings, OlmMachine};
use matrix_sdk_base::RequestedRequiredStates;
use std::time::{Duration, Instant};

const ROOM: &str = "!forged:localhost";
const ALICE: &str = "@alice:localhost";
const BOB: &str = "@bob:localhost";
const ME: &str = "@example:localhost";

type Queue = Arc<Mutex<VecDeque<String>>>;

fn room() -> &'static RoomId {
    room_id!("!forged:localhost")
}

fn message(sender: &str, id: &str, body: &str, ts: u64) -> serde_json::Value {
    json!({"type": "m.room.message", "event_id": id, "sender": sender, "origin_server_ts": ts,
        "content": {"msgtype": "m.text", "body": body}})
}

fn edit(sender: &str, id: &str, target: &str, body: &str, ts: u64) -> serde_json::Value {
    let mut value = message(sender, id, &format!("* {body}"), ts);
    value["content"]["m.mentions"] = json!({"user_ids": [ME]});
    value["content"]["m.new_content"] =
        json!({"msgtype": "m.text", "body": body, "m.mentions": {"user_ids": [ME]}});
    value["content"]["m.relates_to"] = json!({"rel_type": "m.replace", "event_id": target});
    value
}

async fn sync(client: &Client, events: Vec<serde_json::Value>, pos: &str) {
    let mut response = v5::Response::new(pos.to_owned());
    let mut entry = v5::response::Room::default();
    entry.timeline = events
        .into_iter()
        .map(|v| Raw::from_json_string(v.to_string()).unwrap())
        .collect();
    response.rooms.insert(room().to_owned(), entry);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
}

async fn cached(client: &Client, id: &str) -> matrix_sdk::deserialized_responses::TimelineEvent {
    let (cache, _handles) = client.get_room(room()).unwrap().event_cache().await.unwrap();
    let id = EventId::parse(id).unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(event) = cache.find_event(&id).await.unwrap() {
            return event;
        }
        assert!(Instant::now() < deadline, "{id} never reached the event cache");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

fn install(client: &Client) -> Queue {
    let events: Queue = Arc::new(Mutex::new(VecDeque::new()));
    install_event_handlers(
        client,
        Arc::clone(&events),
        Arc::new(Mutex::new(None)),
        Arc::new(Mutex::new(None)),
        Arc::new(Mutex::new(None)),
        Arc::new(syncnotify::SyncNotify::default()),
    );
    events
}

/// Every forwarded timeline event, once `barrier` (synced after the event
/// under test) has been forwarded.
async fn forwarded(events: &Queue, barrier: &str) -> Vec<serde_json::Value> {
    let rows = || -> Vec<serde_json::Value> {
        events
            .lock()
            .unwrap()
            .iter()
            .map(|raw| serde_json::from_str::<serde_json::Value>(raw).unwrap())
            .filter(|e| e["type"] == "timeline_event")
            .map(|e| e["event"].clone())
            .collect()
    };
    let deadline = Instant::now() + Duration::from_secs(5);
    while !rows().iter().any(|e| e["event_id"] == barrier) {
        assert!(Instant::now() < deadline, "{barrier} was never forwarded");
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
    rows()
}

fn edits_of<'a>(rows: &'a [serde_json::Value], target: &str) -> Vec<&'a serde_json::Value> {
    rows.iter().filter(|e| e["replaces_event_id"] == target).collect()
}

async fn seal(alice: &OlmMachine, mut event: serde_json::Value) -> serde_json::Value {
    let content = Raw::from_json_string(event["content"].to_string()).unwrap();
    let sealed = alice.encrypt_room_event_raw(room(), "m.room.message", &content).await.unwrap();
    event["type"] = json!("m.room.encrypted");
    event["content"] = serde_json::from_str(sealed.content.json().get()).unwrap();
    event
}

/// A client holding Alice's room key, and Alice's machine to seal more.
async fn encrypted_room(server: &MatrixMockServer) -> (Client, OlmMachine) {
    use matrix_sdk::ruma::{device_id, user_id};
    let client = server.client_builder().build().await;
    client.encryption().wait_for_e2ee_initialization_tasks().await;
    client.event_cache().subscribe().unwrap();
    let alice = OlmMachine::new(user_id!("@alice:localhost"), device_id!("FIRST")).await;
    alice.share_room_key(room(), std::iter::empty(), EncryptionSettings::default()).await.unwrap();
    let keys = alice.store().export_room_keys(|_| true).await.unwrap();
    {
        let machine = client.olm_machine_for_testing().await;
        machine.as_ref().unwrap().store().import_exported_room_keys(keys, |_, _| {}).await.unwrap();
    }
    (client, alice)
}

async fn plain_room(server: &MatrixMockServer) -> Client {
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    client
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_plaintext_edit_of_an_encrypted_message_never_reaches_the_notification_path() {
    let server = MatrixMockServer::new().await;
    let (client, alice) = encrypted_room(&server).await;
    let events = install(&client);
    let original = seal(&alice, message(ALICE, "$original", "original words", 1000)).await;
    sync(&client, vec![original], "1").await;
    assert!(cached(&client, "$original").await.encryption_info().is_some());
    sync(&client, vec![edit(ALICE, "$forged", "$original", "FORGED words", 2000),
        message(ALICE, "$barrier", "barrier", 3000)], "2").await;
    let rows = forwarded(&events, "$barrier").await;
    let original = rows.iter().find(|e| e["event_id"] == "$original").expect("original forwarded");
    assert_eq!((original["body"].as_str(), original["is_encrypted"].as_bool()),
        (Some("original words"), Some(true)));
    assert!(edits_of(&rows, "$original").is_empty(), "forged edit forwarded: {rows:?}");
    assert!(!rows.iter().any(|e| e.to_string().contains("FORGED")));
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_encrypted_edit_of_an_encrypted_message_still_reaches_the_notification_path() {
    let server = MatrixMockServer::new().await;
    let (client, alice) = encrypted_room(&server).await;
    let events = install(&client);
    let original = seal(&alice, message(ALICE, "$original", "original words", 1000)).await;
    sync(&client, vec![original], "1").await;
    cached(&client, "$original").await;
    let fixed = seal(&alice, edit(ALICE, "$fixed", "$original", "fixed words", 2000)).await;
    sync(&client, vec![fixed, message(ALICE, "$barrier", "barrier", 3000)], "2").await;
    let rows = forwarded(&events, "$barrier").await;
    let edits = edits_of(&rows, "$original");
    assert_eq!(edits.len(), 1, "{rows:?}");
    assert_eq!(edits[0]["new_body"], "fixed words");
    assert_eq!(edits[0]["mentions_me"], true);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_strangers_edit_never_reaches_the_notification_path() {
    let server = MatrixMockServer::new().await;
    let client = plain_room(&server).await;
    let events = install(&client);
    sync(&client, vec![message(ALICE, "$o", "alice's words", 1000)], "1").await;
    cached(&client, "$o").await;
    sync(&client, vec![edit(BOB, "$e", "$o", "bob's words", 2000),
        message(ALICE, "$barrier", "barrier", 3000)], "2").await;
    let rows = forwarded(&events, "$barrier").await;
    assert!(edits_of(&rows, "$o").is_empty(), "stranger's edit forwarded: {rows:?}");
    assert!(!rows.iter().any(|e| e["event_id"] == "$e"));
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_senders_own_edit_still_reaches_the_notification_path() {
    let server = MatrixMockServer::new().await;
    let client = plain_room(&server).await;
    let events = install(&client);
    sync(&client, vec![message(ALICE, "$o", "alice's words", 1000)], "1").await;
    cached(&client, "$o").await;
    sync(&client, vec![edit(ALICE, "$e", "$o", "alice's new words", 2000),
        message(ALICE, "$barrier", "barrier", 3000)], "2").await;
    let rows = forwarded(&events, "$barrier").await;
    let edits = edits_of(&rows, "$o");
    assert_eq!(edits.len(), 1, "{rows:?}");
    assert_eq!(edits[0]["new_body"], "alice's new words");
    assert_eq!(edits[0]["mentions_me"], true);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_edit_of_a_redacted_message_never_reaches_the_notification_path() {
    let server = MatrixMockServer::new().await;
    let client = plain_room(&server).await;
    let events = install(&client);
    let redaction = json!({"type": "m.room.redaction", "event_id": "$r", "sender": ALICE,
        "origin_server_ts": 1500, "redacts": "$o", "content": {"redacts": "$o"}});
    sync(&client, vec![message(ALICE, "$o", "alice's words", 1000), redaction], "1").await;
    let deadline = Instant::now() + Duration::from_secs(5);
    while cached(&client, "$o").await.raw().get_field::<serde_json::Value>("unsigned").ok().flatten()
        .is_none_or(|u| u.get("redacted_because").is_none())
    {
        assert!(Instant::now() < deadline, "the redaction never reached the cache");
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    sync(&client, vec![edit(ALICE, "$e", "$o", "back from the dead", 2000),
        message(ALICE, "$barrier", "barrier", 3000)], "2").await;
    let rows = forwarded(&events, "$barrier").await;
    assert!(edits_of(&rows, "$o").is_empty(), "edit of a redacted message forwarded: {rows:?}");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_fetched_plaintext_edit_of_an_encrypted_message_is_dropped() {
    let server = MatrixMockServer::new().await;
    let (client, alice) = encrypted_room(&server).await;
    let original = seal(&alice, message(ALICE, "$original", "original words", 1000)).await;
    let fixed = seal(&alice, edit(ALICE, "$fixed", "$original", "fixed words", 2500)).await;
    sync(&client, vec![original, edit(ALICE, "$forged", "$original", "FORGED words", 2000), fixed],
        "1").await;
    cached(&client, "$original").await;
    let room = client.get_room(room()).unwrap();
    let verdicts = push_verdict::SyncVerdicts::default();
    let forged = held_event_json(&room, &cached(&client, "$forged").await, true, &verdicts).await;
    assert!(forged.is_none(), "forged edit forwarded: {forged:?}");
    let fixed = held_event_json(&room, &cached(&client, "$fixed").await, true, &verdicts)
        .await
        .expect("the owner's encrypted edit");
    assert_eq!(fixed["new_body"], "fixed words");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_plaintext_edit_of_an_encrypted_message_is_not_seeded_into_activity() {
    use matrix_sdk::ruma::api::client::push::get_notifications::v3::Notification;
    use matrix_sdk::ruma::MilliSecondsSinceUnixEpoch;
    let server = MatrixMockServer::new().await;
    let (client, alice) = encrypted_room(&server).await;
    let original = seal(&alice, message(ALICE, "$original", "original words", 1000)).await;
    sync(&client, vec![original], "1").await;
    cached(&client, "$original").await;
    let notification = |event: serde_json::Value| {
        Notification::new(
            vec![],
            Raw::from_json_string(event.to_string()).unwrap(),
            false,
            room().to_owned(),
            MilliSecondsSinceUnixEpoch(2000u32.into()),
        )
    };
    let entries = crate::rooms::activity_seed_entries(
        &client,
        vec![
            notification(edit(ALICE, "$forged", "$original", "FORGED words", 2000)),
            notification(message(ALICE, "$plain", "hi @example", 2100)),
        ],
        ME,
    )
    .await;
    let ids: Vec<&str> = entries.iter().map(|e| e["event_id"].as_str().unwrap()).collect();
    assert_eq!(ids, ["$plain"]);
}

#[test]
fn without_the_original_only_an_encrypted_edit_or_a_plain_rooms_is_admitted() {
    let raw = Raw::from_json_string(edit(ALICE, "$e", "$o", "words", 2000).to_string()).unwrap();
    assert!(!replacement_admitted_against(None, &raw, None, false));
    assert!(replacement_admitted_against(None, &raw, None, true));
    let sealed = json!({"type": "m.room.encrypted", "event_id": "$o", "sender": ALICE,
        "origin_server_ts": 1000, "content": {"algorithm": "m.megolm.v1.aes-sha2"}});
    let sealed = matrix_sdk::deserialized_responses::TimelineEvent::from_plaintext(
        Raw::from_json_string(sealed.to_string()).unwrap());
    assert!(!replacement_admitted_against(Some(&sealed), &raw, None, true));
}

fn messages_page(events: &[serde_json::Value]) -> matrix_sdk::test_utils::mocks::RoomMessagesResponseTemplate {
    matrix_sdk::test_utils::mocks::RoomMessagesResponseTemplate::default().events(
        events
            .iter()
            .map(|v| serde_json::from_value::<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>>(v.clone()).unwrap())
            .collect::<Vec<_>>(),
    )
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_unread_fetch_pages_past_thirty_events_to_a_mention() {
    let server = MatrixMockServer::new().await;
    let (client, alice) = encrypted_room(&server).await;
    sync(&client, vec![seal(&alice, message(ALICE, "$live", "newest", 9000)).await], "1").await;
    let mut newest_first = Vec::new();
    for i in (0..36u64).rev() {
        let mut event = message(ALICE, &format!("$g{i}"), "gap line", 2000 + i);
        if i == 2 {
            event["content"]["m.mentions"] = json!({"user_ids": [ME]});
        }
        newest_first.push(seal(&alice, event).await);
    }
    newest_first.push(message(ME, "$mine", "my last words", 1000));
    server.mock_room_messages().match_limit(30)
        .ok(messages_page(&newest_first[..30]).end_token("p1")).mount().await;
    server.mock_room_messages().match_from("p1").ok(messages_page(&newest_first[30..])).mount().await;

    let room = client.get_room(room()).unwrap();
    let tail = unread_gap_events(&room, 0).await;
    let ids: Vec<String> = tail.iter().map(|e| e.event_id().unwrap().to_string()).collect();
    assert_eq!(ids.len(), 36, "{ids:?}");
    assert_eq!((ids[0].as_str(), ids[35].as_str()), ("$g0", "$g35"));
    let verdicts = push_verdict::SyncVerdicts::default();
    let mention = held_event_json(&room, &tail[2], true, &verdicts).await.expect("the mention");
    assert_eq!((mention["event_id"].as_str(), mention["mentions_me"].as_bool()), (Some("$g2"), Some(true)));
    assert_eq!(mention["is_decrypted"], true);

    let recent = unread_gap_events(&room, 2020 + syncnotify::TIMESTAMP_ALLOWANCE_MS).await;
    assert_eq!(recent.len(), 16);
    assert_eq!(recent[0].event_id().unwrap().as_str(), "$g20");
}

async fn sync_limited(client: &Client, room: &RoomId, event: serde_json::Value, pos: &str, limited: bool) {
    let mut response = v5::Response::new(pos.to_owned());
    let mut entry = v5::response::Room::default();
    entry.timeline = vec![Raw::from_json_string(event.to_string()).unwrap()];
    entry.limited = limited;
    response.rooms.insert(room.to_owned(), entry);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_limited_sync_reports_a_gap_only_when_it_may_hide_unread_events() {
    let server = MatrixMockServer::new().await;
    let client = plain_room(&server).await;
    let events: Queue = Arc::new(Mutex::new(VecDeque::new()));
    let watch = spawn_unread_gap_watch(&client, &events);
    let gaps = || -> Vec<String> {
        events
            .lock()
            .unwrap()
            .iter()
            .map(|raw| serde_json::from_str::<serde_json::Value>(raw).unwrap())
            .filter(|row| row["type"] == "room_gap")
            .map(|row| row["room_id"].as_str().unwrap().to_owned())
            .collect()
    };
    sync_limited(&client, room_id!("!own:localhost"), message(ME, "$b", "my own", 2000), "1", true).await;
    sync_limited(&client, room_id!("!whole:localhost"), message(ALICE, "$c", "no gap", 3000), "2", false).await;
    sync_limited(&client, room(), message(ALICE, "$a", "after a gap", 1000), "3", true).await;
    sync_limited(&client, room(), message(ALICE, "$d", "the same gap, still arriving", 4000), "4", true).await;
    let done = || events.lock().unwrap().iter().filter(|raw| raw.contains("unread_fetch_done")).count();
    let deadline = Instant::now() + Duration::from_secs(40);
    while done() == 0 {
        assert!(Instant::now() < deadline, "the gap was never fetched");
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(gaps(), [ROOM]);
    assert_eq!(done(), 1);
    watch.abort();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_forged_mention_edit_inside_the_gap_is_not_fetched_as_evidence() {
    let server = MatrixMockServer::new().await;
    let (client, alice) = encrypted_room(&server).await;
    sync(&client, vec![seal(&alice, message(ALICE, "$live", "newest", 9000)).await], "1").await;
    let mut real = message(ALICE, "$real", "a real mention", 4000);
    real["content"]["m.mentions"] = json!({"user_ids": [ME]});
    let page = vec![
        seal(&alice, real).await,
        edit(ALICE, "$forged", "$original", "FORGED words", 3000),
        seal(&alice, message(ALICE, "$original", "original words", 2000)).await,
        message(ME, "$mine", "my last words", 1000),
    ];
    server.mock_room_messages().ok(messages_page(&page)).mount().await;

    let events: Queue = Arc::new(Mutex::new(VecDeque::new()));
    let room = client.get_room(room()).unwrap();
    enqueue_unread_gap_events(&room, 0, &events, || true).await;
    let rows: Vec<serde_json::Value> =
        events.lock().unwrap().iter().map(|raw| serde_json::from_str(raw).unwrap()).collect();
    assert!(!rows.iter().any(|row| row.to_string().contains("FORGED")), "{rows:?}");
    let (done, fetched) = rows.split_last().expect("the fetch reported its end");
    assert_eq!((done["type"].as_str(), done["room_id"].as_str()), (Some("unread_fetch_done"), Some(ROOM)));
    let ids: Vec<&str> = fetched.iter().map(|row| row["event"]["event_id"].as_str().unwrap()).collect();
    assert_eq!(ids, ["$original", "$real"]);
    for row in fetched {
        assert_eq!((row["type"].as_str(), row["event"]["fetched"].as_bool(), row["event"]["backlog"].as_bool()),
            (Some("timeline_event"), Some(true), Some(true)));
    }
    assert_eq!(fetched[1]["event"]["mentions_me"], true);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn dropped_room_updates_sweep_the_rooms_that_hold_unread_events() {
    let server = MatrixMockServer::new().await;
    let client = plain_room(&server).await;
    let mut response = v5::Response::new("1".to_owned());
    let mut unread = v5::response::Room::default();
    unread.timeline = vec![Raw::from_json_string(message(ALICE, "$a", "unseen", 1000).to_string()).unwrap()];
    unread.unread_notifications.notification_count = Some(3u32.into());
    response.rooms.insert(room().to_owned(), unread);
    let mut muted = v5::response::Room::default();
    muted.timeline = vec![Raw::from_json_string(message(ALICE, "$m", "unseen", 1500).to_string()).unwrap()];
    muted.unread_notifications.notification_count = Some(3u32.into());
    response.rooms.insert(room_id!("!muted:localhost").to_owned(), muted);
    response.extensions.account_data.global.push(
        Raw::from_json_string(
            json!({"type": "m.push_rules", "content": {"global": {"override": [{
                "rule_id": "!muted:localhost", "default": false, "enabled": true, "actions": [],
                "conditions": [{"kind": "event_match", "key": "room_id", "pattern": "!muted:localhost"}],
            }]}}})
            .to_string(),
        )
        .unwrap(),
    );
    let mut read = v5::response::Room::default();
    read.timeline = vec![Raw::from_json_string(message(ME, "$b", "my own", 2000).to_string()).unwrap()];
    response.rooms.insert(room_id!("!read:localhost").to_owned(), read);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();

    let muted = client.get_room(room_id!("!muted:localhost")).unwrap();
    assert!(may_hold_unread(&muted), "the muted room must be skipped for its mute, not for being read");

    let (updates, receiver) = tokio::sync::broadcast::channel(1);
    for _ in 0..3 {
        updates.send(matrix_sdk::sync::RoomUpdates::default()).unwrap();
    }
    let events: Queue = Arc::new(Mutex::new(VecDeque::new()));
    let watch = spawn_unread_gap_watch_on(&client, &events, receiver);
    let rows = || -> Vec<serde_json::Value> {
        events.lock().unwrap().iter().map(|raw| serde_json::from_str(raw).unwrap()).collect()
    };
    let deadline = Instant::now() + Duration::from_secs(40);
    while !rows().iter().any(|row| row["type"] == "unread_fetch_done") {
        assert!(Instant::now() < deadline, "the lag was never swept: {:?}", rows());
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    tokio::time::sleep(Duration::from_millis(500)).await;
    let rows = rows();
    let gaps: Vec<&str> = rows.iter().filter(|row| row["type"] == "room_gap")
        .map(|row| row["room_id"].as_str().unwrap()).collect();
    assert_eq!(gaps, [ROOM]);
    let traces: Vec<&str> = rows.iter().filter(|row| row["type"] == "rust_trace")
        .map(|row| row["line"].as_str().unwrap()).collect();
    assert_eq!(traces, ["room updates lagged, dropped 2: sweeping unread rooms"]);
    watch.abort();
    drop(updates);
}
