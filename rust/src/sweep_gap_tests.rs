use super::*;
use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
use matrix_sdk::test_utils::mocks::{MatrixMockServer, RoomMessagesResponseTemplate};
use matrix_sdk_base::RequestedRequiredStates;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

const ROOM: &str = "!dm:localhost";

fn message(id: &str, body: &str, ts: u64) -> serde_json::Value {
    from("@friend:localhost", id, body, ts)
}

fn from(sender: &str, id: &str, body: &str, ts: u64) -> serde_json::Value {
    serde_json::json!({"type": "m.room.message", "event_id": id, "room_id": ROOM,
        "sender": sender, "origin_server_ts": ts,
        "content": {"msgtype": "m.text", "body": body}})
}

async fn sync(client: &matrix_sdk::Client, events: Vec<serde_json::Value>, pos: &str, gap: Option<&str>) {
    let mut response = v5::Response::new(pos.to_owned());
    let mut room = v5::response::Room::default();
    room.timeline = events
        .into_iter()
        .map(|v| Raw::from_json_string(v.to_string()).unwrap())
        .collect();
    room.limited = gap.is_some();
    room.prev_batch = gap.map(str::to_owned);
    response.rooms.insert(room_id!("!dm:localhost").to_owned(), room);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
}

fn open_gate() -> GapGate {
    GapGate {
        start_delay: std::time::Duration::ZERO,
        held: Arc::new(|| false),
        stop: Arc::new(AtomicBool::new(false)),
    }
}

fn redaction(id: &str, target: &str, ts: u64) -> serde_json::Value {
    serde_json::json!({"type": "m.room.redaction", "event_id": id, "room_id": ROOM,
        "sender": "@friend:localhost", "origin_server_ts": ts, "redacts": target,
        "content": {"redacts": target}})
}

async fn wait_indexed(index: &Arc<Mutex<Option<SearchIndex>>>, id: &str) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !indexed(index, id) {
        assert!(std::time::Instant::now() < deadline, "{id} never indexed");
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
}

fn indexed(index: &Arc<Mutex<Option<SearchIndex>>>, id: &str) -> bool {
    index.lock().unwrap().as_ref().unwrap().contains(id).unwrap()
}

async fn sweep_until(
    client: &matrix_sdk::Client,
    index: &Arc<Mutex<Option<SearchIndex>>>,
    id: &str,
) {
    let stop = AtomicBool::new(false);
    for _ in 0..100 {
        sweep(client, index, &stop).await;
        if indexed(index, id) {
            return;
        }
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    panic!("{id} never reached the index");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn messages_behind_a_limited_sync_are_indexed_by_the_next_sweep() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));

    sync(&client, vec![message("$m1", "an older line", 1000)], "1", None).await;
    sweep_until(&client, &index, "$m1").await;

    let gap: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> = [
        message("$m3", "the zephyrine plan", 3000),
        message("$m2", "a second gap line", 2000),
        message("$m1", "an older line", 1000),
    ]
    .into_iter()
    .map(|v| serde_json::from_value(v).unwrap())
    .collect();
    server
        .mock_room_messages()
        .ok(RoomMessagesResponseTemplate::default().events(gap))
        .mount()
        .await;
    sync(&client, vec![message("$m4", "the newest line", 4000)], "2", Some("gap-token")).await;
    sweep_until(&client, &index, "$m4").await;

    assert!(indexed(&index, "$m3"), "a message that arrived in the sync gap was never indexed");
    assert!(indexed(&index, "$m2"));
    let hits = index.lock().unwrap().as_ref().unwrap().search("zephyrine", "", 10, 0).unwrap();
    assert_eq!(hits.len(), 1);
}

#[test]
fn rooms_active_since_the_last_sweep_come_first() {
    let rooms: Vec<(usize, u64)> = (0..100).map(|i| (i, if i == 97 { 5_000 } else { 10 })).collect();
    let (order, moved) = sweep_order(rooms.clone(), 1_000, 0, 40);
    assert_eq!(order.len(), 40);
    assert_eq!(order[0], 97, "the room with new messages waited for its turn");
    assert_eq!(moved, 39);
    let (first, moved) = sweep_order(rooms, 0, 0, 40);
    assert_eq!(first[0], 97, "the first sweep after start-up skipped the newest room");
    assert_eq!(moved, 0);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_own_message_is_indexed_as_sync_delivers_it_without_a_sweep() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _guard = register_live_index_handler(&client, &index);
    let me = client.user_id().unwrap().to_string();

    sync(&client, vec![from(&me, "$own", "Issue is still there", 1000)], "1", None).await;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !indexed(&index, "$own") {
        assert!(std::time::Instant::now() < deadline, "the user's own message was never indexed");
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    let hits = index.lock().unwrap().as_ref().unwrap().search("issue is still", "", 10, 0).unwrap();
    assert_eq!(hits.len(), 1);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_limited_sync_gap_is_filled_without_waiting_for_a_sweep() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);
    let events = Arc::new(Mutex::new(std::collections::VecDeque::new()));
    let filler = spawn_gap_filler(&client, &index, open_gate(), &events);

    sync(&client, vec![message("$m1", "an older line", 1000)], "1", None).await;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !indexed(&index, "$m1") {
        assert!(std::time::Instant::now() < deadline, "$m1 never indexed");
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    let gap: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> = [
        message("$m3", "the zephyrine plan", 3000),
        message("$m2", "a second gap line", 2000),
        message("$m1", "an older line", 1000),
    ]
    .into_iter()
    .map(|v| serde_json::from_value(v).unwrap())
    .collect();
    server
        .mock_room_messages()
        .ok(RoomMessagesResponseTemplate::default().events(gap))
        .mount()
        .await;
    sync(&client, vec![message("$m4", "the newest line", 4000)], "2", Some("gap-token")).await;

    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !(indexed(&index, "$m3") && indexed(&index, "$m2") && indexed(&index, "$m4")) {
        assert!(
            std::time::Instant::now() < deadline,
            "messages a limited sync never delivered were not indexed"
        );
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    filler.abort();
    assert!(
        events.lock().unwrap().iter().any(|e| e.contains("\"source\":\"gap\"")),
        "the gap fill was not reported"
    );
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_message_redacted_in_the_same_sync_is_never_indexed() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);
    let _redactions = register_redaction_handler(&client, &index);

    sync(
        &client,
        vec![message("$secret", "a line nobody may find", 1000), redaction("$r", "$secret", 1001)],
        "1",
        None,
    )
    .await;
    sync(&client, vec![message("$barrier", "a later line", 2000)], "2", None).await;
    wait_indexed(&index, "$barrier").await;
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    assert!(!indexed(&index, "$secret"), "a message redacted in the same sync stayed searchable");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn redacting_the_shown_edit_falls_back_to_the_previous_one_without_a_sweep() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);
    let _redactions = register_redaction_handler(&client, &index);

    sync(&client, vec![message("$m0", "an older line", 500)], "0", None).await;
    server.mock_room_state_encryption().plain().mount().await;
    client
        .get_room(room_id!("!dm:localhost"))
        .unwrap()
        .latest_encryption_state()
        .await
        .unwrap();
    let edit = |id: &str, body: &str, ts: u64| {
        serde_json::json!({"type": "m.room.message", "event_id": id, "room_id": ROOM,
            "sender": "@friend:localhost", "origin_server_ts": ts,
            "content": {"msgtype": "m.text", "body": format!("* {body}"),
                "m.new_content": {"msgtype": "m.text", "body": body},
                "m.relates_to": {"rel_type": "m.replace", "event_id": "$o"}}})
    };
    sync(&client, vec![message("$o", "kappa original", 1000)], "1", None).await;
    sync(&client, vec![edit("$e1", "lambda interim", 2000)], "2", None).await;
    sync(&client, vec![edit("$e2", "mu final", 3000)], "3", None).await;
    let hits = |q: &str| index.lock().unwrap().as_ref().unwrap().search(q, "", 10, 0).unwrap().len();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while hits("mu final") == 0 {
        assert!(std::time::Instant::now() < deadline, "the last edit was never indexed");
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }

    sync(&client, vec![redaction("$r", "$e2", 3500)], "4", None).await;
    sync(&client, vec![message("$barrier", "a later line", 4000)], "5", None).await;
    wait_indexed(&index, "$barrier").await;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while hits("lambda interim") == 0 && std::time::Instant::now() < deadline {
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    assert_eq!(hits("mu final"), 0, "the redacted edit is still searchable");
    assert_eq!(hits("lambda interim"), 1, "the message did not fall back to the previous edit");
    assert_eq!(hits("kappa original"), 0);
    assert!(indexed(&index, "$o"));

    sync(&client, vec![redaction("$r1", "$e1", 4500)], "6", None).await;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while hits("kappa original") == 0 && std::time::Instant::now() < deadline {
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    assert_eq!(hits("lambda interim"), 0);
    assert_eq!(hits("kappa original"), 1, "the message did not fall back to its own text");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_gap_fill_applies_the_redactions_it_fetches() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);
    let events = Arc::new(Mutex::new(std::collections::VecDeque::new()));
    let filler = spawn_gap_filler(&client, &index, open_gate(), &events);

    sync(&client, vec![message("$m1", "an older line", 1000)], "1", None).await;
    wait_indexed(&index, "$m1").await;
    let mut gone = message("$m3", "an already redacted line", 3000);
    gone["unsigned"] = serde_json::json!({"redacted_because": redaction("$r3", "$m3", 3100)});
    let gap: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> = [
        redaction("$r1", "$m1", 3500),
        gone,
        message("$m2", "a second gap line", 2000),
        message("$m0", "the oldest line", 500),
    ]
    .into_iter()
    .map(|v| serde_json::from_value(v).unwrap())
    .collect();
    server
        .mock_room_messages()
        .ok(RoomMessagesResponseTemplate::default().events(gap))
        .mount()
        .await;
    sync(&client, vec![message("$m4", "the newest line", 4000)], "2", Some("gap-token")).await;
    wait_indexed(&index, "$m2").await;
    filler.abort();
    assert!(!indexed(&index, "$m1"), "a redaction on a fetched page was ignored");
    assert!(!indexed(&index, "$m3"), "an already redacted event was indexed");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_gap_fill_applies_edits_paged_before_their_original() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);
    let events = Arc::new(Mutex::new(std::collections::VecDeque::new()));
    let filler = spawn_gap_filler(&client, &index, open_gate(), &events);

    sync(&client, vec![message("$m1", "an older line", 1000)], "1", None).await;
    wait_indexed(&index, "$m1").await;
    server.mock_room_state_encryption().plain().mount().await;
    client
        .get_room(room_id!("!dm:localhost"))
        .unwrap()
        .latest_encryption_state()
        .await
        .unwrap();
    let edit = |id: &str, body: &str, ts: u64| {
        serde_json::json!({"type": "m.room.message", "event_id": id, "room_id": ROOM,
            "sender": "@friend:localhost", "origin_server_ts": ts,
            "content": {"msgtype": "m.text", "body": format!("* {body}"),
                "m.new_content": {"msgtype": "m.text", "body": body},
                "m.relates_to": {"rel_type": "m.replace", "event_id": "$o"}}})
    };
    let raws = |values: Vec<serde_json::Value>| -> Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> {
        values.into_iter().map(|v| serde_json::from_value(v).unwrap()).collect()
    };
    server
        .mock_room_messages()
        .match_from("gap-token")
        .ok(RoomMessagesResponseTemplate::default()
            .events(raws(vec![edit("$e2", "the corrected plan", 3000), edit("$e1", "an interim plan", 2500)]))
            .end_token("p1"))
        .mount()
        .await;
    server
        .mock_room_messages()
        .match_from("p1")
        .ok(RoomMessagesResponseTemplate::default().events(raws(vec![message("$o", "the first plan", 2000)])))
        .mount()
        .await;
    sync(&client, vec![message("$m4", "the newest line", 4000)], "2", Some("gap-token")).await;
    wait_indexed(&index, "$o").await;
    filler.abort();
    let ix = index.lock().unwrap();
    let ix = ix.as_ref().unwrap();
    assert_eq!(ix.search("corrected plan", "", 10, 0).unwrap().len(), 1, "the edit paged first was lost");
    assert!(ix.search("first plan", "", 10, 0).unwrap().is_empty());
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_plaintext_edit_in_an_encrypted_room_is_not_indexed() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);

    let mut response = v5::Response::new("0".to_owned());
    let mut room = v5::response::Room::default();
    room.required_state = vec![Raw::from_json_string(
        serde_json::json!({"type": "m.room.encryption", "state_key": "", "event_id": "$enc",
            "sender": "@friend:localhost", "origin_server_ts": 1,
            "content": {"algorithm": "m.megolm.v1.aes-sha2"}})
        .to_string(),
    )
    .unwrap()];
    response.rooms.insert(room_id!("!dm:localhost").to_owned(), room);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
    assert!(client.get_room(room_id!("!dm:localhost")).unwrap().encryption_state().is_encrypted());

    let forged = serde_json::json!({"type": "m.room.message", "event_id": "$e", "room_id": ROOM,
        "sender": "@friend:localhost", "origin_server_ts": 2000,
        "content": {"msgtype": "m.text", "body": "* the forged plan",
            "m.new_content": {"msgtype": "m.text", "body": "the forged plan"},
            "m.relates_to": {"rel_type": "m.replace", "event_id": "$o"}}});
    sync(&client, vec![message("$o", "the first plan", 1000), forged], "1", None).await;
    sync(&client, vec![message("$barrier", "a later line", 3000)], "2", None).await;
    wait_indexed(&index, "$barrier").await;
    let ix = index.lock().unwrap();
    let ix = ix.as_ref().unwrap();
    assert!(ix.search("forged plan", "", 10, 0).unwrap().is_empty(), "a plaintext edit rewrote an encrypted room's row");
    assert_eq!(ix.search("first plan", "", 10, 0).unwrap().len(), 1);
}

#[test]
fn the_gap_queue_is_first_in_first_out_with_the_newest_token() {
    let mut queue = GapQueue::default();
    queue.push("!b:x".into(), "b1".into(), 1);
    queue.push("!a:x".into(), "a1".into(), 1);
    queue.push("!b:x".into(), "b2".into(), 1);
    let zero = std::time::Duration::ZERO;
    assert!(matches!(queue.pop(zero), GapNext::Ready(r, t, 1) if r == "!b:x" && t == "b2"));
    assert!(matches!(queue.pop(zero), GapNext::Ready(r, t, 1) if r == "!a:x" && t == "a1"));
    assert!(matches!(queue.pop(zero), GapNext::Empty));
}

#[test]
fn the_gap_queue_holds_a_room_until_its_limited_syncs_settle() {
    let mut queue = GapQueue::default();
    queue.push("!busy:x".into(), "t1".into(), 1);
    let settle = std::time::Duration::from_millis(400);
    assert!(matches!(queue.pop(settle), GapNext::Wait(left) if left <= settle));
    std::thread::sleep(std::time::Duration::from_millis(100));
    queue.push("!busy:x".into(), "t2".into(), 1);
    std::thread::sleep(std::time::Duration::from_millis(100));
    assert!(matches!(queue.pop(settle), GapNext::Wait(_)), "a newer limited sync must restart the wait");
    std::thread::sleep(std::time::Duration::from_millis(400));
    assert!(matches!(queue.pop(settle), GapNext::Ready(_, t, 1) if t == "t2"));
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_deep_walk_releases_the_history_it_paged_into_memory() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    sync(&client, vec![message("$live", "the newest line", 1_000_000)], "1", Some("p0")).await;

    for page in 0..3u64 {
        let chunk: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> = (0..100u64)
            .map(|i| {
                let ts = 900_000 - page * 1000 - i;
                serde_json::from_value(message(&format!("$p{page}e{i}"), "old history", ts)).unwrap()
            })
            .collect();
        let template = RoomMessagesResponseTemplate::default().events(chunk);
        let template = if page < 2 { template.end_token(format!("p{}", page + 1)) } else { template };
        server.mock_room_messages().match_from(&format!("p{page}")).ok(template).mount().await;
    }

    let room = client.get_room(room_id!("!dm:localhost")).unwrap();
    let stop = AtomicBool::new(false);
    let outcome = deep_index_room_gated(&room, &index, &StopOnly(&stop), DEEP_MAX_PAGES).await.unwrap();
    assert!(outcome.reached_start);
    assert!(indexed(&index, "$p2e99"));

    let (room_cache, _handles) = room.event_cache().await.unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        let held = room_cache.events().await.unwrap().len();
        if held < 100 {
            break;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "{held} paged events still held in memory after the walk"
        );
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_sweep_releases_what_sync_piled_up_in_a_room_nobody_opened() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    for batch in 0..4u64 {
        let events = (0..100u64)
            .map(|i| message(&format!("$s{batch}e{i}"), "busy room line", 10_000 + batch * 100 + i))
            .collect();
        sync(&client, events, &batch.to_string(), None).await;
    }
    let room = client.get_room(room_id!("!dm:localhost")).unwrap();
    let (room_cache, _handles) = room.event_cache().await.unwrap();
    let piled = room_cache.events().await.unwrap().len();
    assert!(piled >= 300, "only {piled} synced events in memory");

    sweep_until(&client, &index, "$s3e99").await;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    loop {
        let held = room_cache.events().await.unwrap().len();
        if held < piled {
            break;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "{held} synced events still held in memory after a sweep"
        );
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
}

#[tokio::test]
async fn search_index_matches_derived_fixtures() {
    use crate::derived_fixtures::{scenarios, Projection, ORIGINAL, OTHER_ROOM, ROOM as HOME_ROOM};
    use matrix_sdk::ruma::RoomId;

    let server = MatrixMockServer::new().await;
    let mut mismatches = Vec::new();
    for case in scenarios() {
        let client = server.client_builder().build().await;
        let encryption = server.mock_room_state_encryption();
        let _scope = if case.original_encrypted { encryption.encrypted() } else { encryption.plain() }
            .mount_as_scoped().await;
        let index = SearchIndex::open_in_memory().unwrap();
        for room_id in [HOME_ROOM, OTHER_ROOM] {
            let events: Vec<_> = case.history(room_id).map(|(raw, enc)| (raw.clone(), enc)).collect();
            if events.is_empty() {
                continue;
            }
            let room = server.sync_joined_room(&client, &RoomId::parse(room_id).unwrap()).await;
            room.latest_encryption_state().await.unwrap();
            assert_eq!(room.encryption_state().is_encrypted(), case.original_encrypted, "{}", case.name);
            if let Some(winner) = &case.before_redaction {
                let original = case.original.as_ref().unwrap();
                let batch = batch_from(&room, [(original.clone(), false), (winner.clone(), false)].into_iter()).await;
                write_batch(&index, room_id, &batch).unwrap();
                let body = winner["content"]["m.new_content"]["body"].as_str().unwrap();
                let hits = index.search(body, room_id, 10, 0).unwrap();
                assert_eq!(hits.len(), 1, "{}", case.name);
                assert_eq!(hits[0].event_id, ORIGINAL, "{}", case.name);
            }
            let batch = batch_from(&room, events.into_iter()).await;
            write_batch(&index, room_id, &batch).unwrap();
        }

        let want = case.content_for(Projection::Search);
        let hits = index.search("fixture", "", 10, 0).unwrap();
        let got: Vec<_> = hits
            .iter()
            .map(|h| (h.event_id.clone(), h.room_id.clone(), h.body.clone(), h.msgtype.clone(), h.filename.clone()))
            .collect();
        let expected: Vec<_> = want
            .map(|c| {
                (
                    ORIGINAL.to_owned(),
                    HOME_ROOM.to_owned(),
                    c["body"].as_str().unwrap().to_owned(),
                    c["msgtype"].as_str().unwrap().to_owned(),
                    c["filename"].as_str().unwrap_or("").to_owned(),
                )
            })
            .into_iter()
            .collect();
        let indexed = index.stats().unwrap().messages;
        if got != expected || indexed != expected.len() as i64 {
            mismatches.push(format!("{}: indexed {indexed}, got {got:?}, expected {expected:?}", case.name));
        }
    }
    assert!(mismatches.is_empty(), "search diverges from the shared fixture:\n{}", mismatches.join("\n"));
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_room_still_receiving_limited_syncs_is_not_paged() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _live = register_live_index_handler(&client, &index);
    let events = Arc::new(Mutex::new(std::collections::VecDeque::new()));
    let filler = spawn_gap_filler(&client, &index, open_gate(), &events);

    sync(&client, vec![message("$m1", "an older line", 1000)], "1", None).await;
    wait_indexed(&index, "$m1").await;
    let page: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> =
        vec![serde_json::from_value(message("$m1", "an older line", 1000)).unwrap()];
    server
        .mock_room_messages()
        .ok(RoomMessagesResponseTemplate::default().events(page))
        .mount()
        .await;
    let pages = || async {
        server
            .server()
            .received_requests()
            .await
            .unwrap_or_default()
            .iter()
            .filter(|r| r.url.path().ends_with("/messages"))
            .count()
    };

    for n in 2..6u64 {
        let id = format!("$m{n}");
        sync(&client, vec![message(&id, "a burst line", n * 1000)], &n.to_string(), Some("gap")).await;
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    }
    assert_eq!(pages().await, 0, "the room was paged while its limited syncs were still arriving");

    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(6);
    while pages().await == 0 {
        assert!(std::time::Instant::now() < deadline, "the settled gap was never filled");
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    tokio::time::sleep(std::time::Duration::from_millis(800)).await;
    filler.abort();
    assert_eq!(pages().await, 1, "one burst is one fill");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn room_scan_receipt_survives_a_refused_page_and_targeted_repair() {
    use crate::indexall::{Control, ScanState};
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    sync(&client, vec![message("$live", "a newer message", 2000)], "1", Some("page")).await;
    let older: Raw<matrix_sdk::ruma::events::AnyTimelineEvent> =
        serde_json::from_value(message("$old", "an earlier message", 1000)).unwrap();
    server.mock_room_messages().match_from("page")
        .ok(RoomMessagesResponseTemplate::default().events(vec![older])).mount().await;
    let room = client.get_room(room_id!("!dm:localhost")).unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let control = Arc::new(Control::new(Path::new("")));
    let gate = control.manual_gate(Arc::new(AtomicBool::new(false)), 1);
    index.lock().unwrap().as_ref().unwrap().conn.execute_batch(
        "CREATE TRIGGER refuse_old BEFORE INSERT ON messages WHEN new.event_id = '$old'
         BEGIN SELECT RAISE(ABORT, 'write refused'); END;"
    ).unwrap();
    assert!(deep_index_room_gated(&room, &index, &gate, 2).await.unwrap_err().storage);
    let failed = control.room_receipt(ROOM);
    assert_eq!(failed.state, ScanState::StorageFailed);
    assert_eq!(failed.pages, 0);
    assert!(!failed.reached_start);
    assert!(indexed(&index, "$live"));
    assert!(!indexed(&index, "$old"));
    index.lock().unwrap().as_ref().unwrap().conn.execute_batch("DROP TRIGGER refuse_old").unwrap();
    deep_index_room_gated(&room, &index, &gate, 2).await.unwrap();
    let repaired = control.room_receipt(ROOM);
    assert_eq!(repaired.state, ScanState::Covered);
    assert_eq!(repaired.oldest_ms, Some(1000));
    assert!(repaired.scanned_ms > 0);
    assert!(indexed(&index, "$old"));
    assert_eq!(control.room_receipt("!other:localhost").state, ScanState::Unscanned);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn bounded_room_scans_distinguish_page_key_and_fetch_gaps() {
    use crate::indexall::{Control, ScanState};
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    sync(&client, vec![message("$recent", "recent", 2000)], "1", Some("older")).await;
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let control = Arc::new(Control::new(Path::new("")));
    let gate = control.manual_gate(Arc::new(AtomicBool::new(false)), 1);
    let room = client.get_room(room_id!("!dm:localhost")).unwrap();
    deep_index_room_gated(&room, &index, &gate, 0).await.unwrap();
    assert_eq!(control.room_receipt(ROOM).state, ScanState::Capped);
    {
        let _error = server.mock_room_messages().match_from("older")
            .error_unrecognized().mount_as_scoped().await;
        assert!(deep_index_room_gated(&room, &index, &gate, 1).await.is_err());
        assert_eq!(control.room_receipt(ROOM).state, ScanState::FetchFailed);
    }
    let locked: Raw<matrix_sdk::ruma::events::AnyTimelineEvent> =
        serde_json::from_value(serde_json::json!({
            "type": "m.room.encrypted", "event_id": "$locked", "room_id": ROOM,
            "sender": "@friend:localhost", "origin_server_ts": 1000,
            "content": {"algorithm": "m.megolm.v1.aes-sha2", "session_id": "missing",
                        "ciphertext": "AwgAEpAB", "device_id": "TEST", "sender_key": "missing"}
        })).unwrap();
    server.mock_room_messages().match_from("older")
        .ok(RoomMessagesResponseTemplate::default().events(vec![locked])).mount().await;
    deep_index_room_gated(&room, &index, &gate, 2).await.unwrap();
    let receipt = control.room_receipt(ROOM);
    assert_eq!(receipt.state, ScanState::WaitingForKeys);
    assert_eq!(receipt.undecryptable, 1);
    assert!(receipt.reached_start);
    assert!(!indexed(&index, "$locked"));
    let _other_walk = control.start_scan(ROOM).unwrap();
    let busy = deep_index_room_gated(&room, &index, &gate, 2).await.unwrap();
    assert!(busy.busy && busy.interrupted);
    assert_eq!(control.room_receipt(ROOM).state, ScanState::Scanning);
}
