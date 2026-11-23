use super::*;
use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
use matrix_sdk::test_utils::mocks::MatrixMockServer;
use matrix_sdk_base::RequestedRequiredStates;
use std::time::Duration;

async fn sync(client: &Client, id: &str) {
    let room = room_id!("!burst:localhost");
    let event = json!({"type": "m.room.message", "event_id": id, "sender": "@alice:localhost",
        "origin_server_ts": 1000, "content": {"msgtype": "m.text", "body": id}});
    let mut response = v5::Response::new(id.to_owned());
    let mut entry = v5::response::Room::default();
    entry.timeline = vec![Raw::from_json_string(event.to_string()).unwrap()];
    response.rooms.insert(room.to_owned(), entry);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
}

// Single-threaded and unconstrained so nothing else runs between the syncs.
#[tokio::test]
async fn a_sync_burst_longer_than_the_update_channel_keeps_the_cached_history() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();

    sync(&client, "$kept").await;
    tokio::time::sleep(Duration::from_millis(200)).await;
    let room = client.get_room(room_id!("!burst:localhost")).unwrap();
    let (cache, _handles) = room.event_cache().await.unwrap();
    let kept = EventId::parse("$kept").unwrap();
    assert!(cache.find_event(&kept).await.unwrap().is_some());

    tokio::task::unconstrained(async {
        for n in 0..40 {
            sync(&client, &format!("$burst{n}")).await;
        }
    })
    .await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    assert!(cache.find_event(&kept).await.unwrap().is_some(), "the burst cleared the room");
    let last = EventId::parse("$burst39").unwrap();
    assert!(cache.find_event(&last).await.unwrap().is_some());
}
