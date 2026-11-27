use std::collections::HashSet;
use std::sync::Arc;

use matrix_sdk_ui::{
    eyeball_im::{Vector, VectorDiff},
    timeline::TimelineItem,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RowKind {
    Virtual,
    OwnLocal,
    OwnRemote,
    Other,
}

pub(crate) trait OrderedRow: Clone {
    fn row_id(&self) -> String;
    fn row_kind(&self) -> RowKind;
}

impl OrderedRow for Arc<TimelineItem> {
    fn row_id(&self) -> String {
        self.unique_id().0.clone()
    }

    fn row_kind(&self) -> RowKind {
        match self.as_event() {
            None => RowKind::Virtual,
            Some(event) if !event.is_own() => RowKind::Other,
            Some(event) if event.is_local_echo() => RowKind::OwnLocal,
            Some(_) => RowKind::OwnRemote,
        }
    }
}

/// Translates SDK timeline diffs into the order rows are shown in.
///
/// The send queue files each sent event into the event cache at once, and a
/// later sync carrying only our own events moves the echo it repeats to the
/// back (matrix-sdk 0.18 `filter_duplicate_events` never treats own-only
/// batches as duplicates), past anything we sent after it. An own event the
/// SDK removes and re-adds in one batch therefore keeps its row when only our
/// own or virtual rows lie between the two places.
pub(crate) struct SendOrder {
    sdk: Vec<String>,
    shown: Vec<(String, RowKind)>,
    aligned: bool,
    lost: bool,
}

impl SendOrder {
    pub(crate) fn new<T: OrderedRow>(items: &Vector<T>) -> Self {
        let mut order = Self { sdk: Vec::new(), shown: Vec::new(), aligned: true, lost: false };
        order.reset(items);
        order
    }

    fn reset<T: OrderedRow>(&mut self, items: &Vector<T>) {
        self.shown = items.iter().map(|item| (item.row_id(), item.row_kind())).collect();
        self.sdk = self.shown.iter().map(|(id, _)| id.clone()).collect();
        self.aligned = true;
        self.lost = false;
    }

    fn shown_at(&self, id: &str) -> usize {
        self.shown.iter().position(|(shown, _)| shown == id).unwrap_or(self.shown.len())
    }

    fn display_of(&self, sdk_index: usize) -> usize {
        if self.aligned {
            return sdk_index;
        }
        match self.sdk.get(sdk_index) {
            Some(id) => self.shown_at(id),
            None => self.shown.len(),
        }
    }

    pub(crate) fn translate<T: OrderedRow>(
        &mut self,
        diffs: Vec<VectorDiff<T>>,
    ) -> Vec<VectorDiff<T>> {
        let mut out = Vec::with_capacity(diffs.len());
        let mut ghosts = HashSet::new();
        for diff in diffs {
            if self.lost {
                match &diff {
                    VectorDiff::Reset { values } => self.reset(values),
                    VectorDiff::Clear => self.reset(&Vector::<T>::new()),
                    _ => {}
                }
                out.push(diff);
                continue;
            }
            self.step(diff, &mut ghosts, &mut out);
        }
        if !self.lost {
            for id in ghosts {
                let at = self.shown_at(&id);
                self.shown.remove(at);
                out.push(VectorDiff::Remove { index: at });
            }
            if !self.aligned {
                self.aligned = self.shown.len() == self.sdk.len()
                    && self.shown.iter().zip(&self.sdk).all(|((shown, _), sdk)| shown == sdk);
            }
        }
        out
    }

    fn step<T: OrderedRow>(
        &mut self,
        diff: VectorDiff<T>,
        ghosts: &mut HashSet<String>,
        out: &mut Vec<VectorDiff<T>>,
    ) {
        let len = self.sdk.len();
        match diff {
            VectorDiff::Append { values } => {
                for value in values.iter() {
                    self.sdk.push(value.row_id());
                    self.shown.push((value.row_id(), value.row_kind()));
                }
                out.push(VectorDiff::Append { values });
            }
            VectorDiff::PushFront { value } => {
                self.sdk.insert(0, value.row_id());
                self.shown.insert(0, (value.row_id(), value.row_kind()));
                out.push(VectorDiff::PushFront { value });
            }
            VectorDiff::PushBack { value } => self.insert(len, value, true, ghosts, out),
            VectorDiff::Insert { index, value } if index <= len => {
                self.insert(index, value, false, ghosts, out);
            }
            VectorDiff::Set { index, value } if index < len => {
                let at = self.display_of(index);
                self.sdk[index] = value.row_id();
                match self.shown.get_mut(at) {
                    Some(slot) => *slot = (value.row_id(), value.row_kind()),
                    None => self.lost = true,
                }
                out.push(VectorDiff::Set { index: at, value });
            }
            VectorDiff::Remove { index } if index < len => {
                if let Some(at) = self.remove(index, ghosts) {
                    out.push(VectorDiff::Remove { index: at });
                }
            }
            VectorDiff::PopFront if len > 0 => {
                if let Some(at) = self.remove(0, ghosts) {
                    out.push(if at == 0 { VectorDiff::PopFront } else { VectorDiff::Remove { index: at } });
                }
            }
            VectorDiff::PopBack if len > 0 => {
                if let Some(at) = self.remove(len - 1, ghosts) {
                    out.push(if at == self.shown.len() {
                        VectorDiff::PopBack
                    } else {
                        VectorDiff::Remove { index: at }
                    });
                }
            }
            VectorDiff::Truncate { length } if length <= len => {
                if self.aligned {
                    self.sdk.truncate(length);
                    self.shown.truncate(length);
                    out.push(VectorDiff::Truncate { length });
                } else {
                    for index in (length..len).rev() {
                        if let Some(at) = self.remove(index, ghosts) {
                            out.push(VectorDiff::Remove { index: at });
                        }
                    }
                }
            }
            VectorDiff::Clear => {
                ghosts.clear();
                self.reset(&Vector::<T>::new());
                out.push(VectorDiff::Clear);
            }
            VectorDiff::Reset { values } => {
                ghosts.clear();
                self.reset(&values);
                out.push(VectorDiff::Reset { values });
            }
            invalid => {
                self.lost = true;
                out.push(invalid);
            }
        }
    }

    fn remove(&mut self, sdk_index: usize, ghosts: &mut HashSet<String>) -> Option<usize> {
        let at = self.display_of(sdk_index);
        let id = self.sdk.remove(sdk_index);
        match self.shown.get(at) {
            Some((_, RowKind::OwnRemote)) => {
                ghosts.insert(id);
                self.aligned = false;
                None
            }
            Some(_) => {
                self.shown.remove(at);
                Some(at)
            }
            None => {
                self.lost = true;
                Some(at)
            }
        }
    }

    fn insert<T: OrderedRow>(
        &mut self,
        sdk_index: usize,
        value: T,
        push_back: bool,
        ghosts: &mut HashSet<String>,
        out: &mut Vec<VectorDiff<T>>,
    ) {
        let id = value.row_id();
        let kind = value.row_kind();
        let mut at = self.display_of(sdk_index);
        self.sdk.insert(sdk_index, id.clone());
        if ghosts.remove(&id) {
            let ghost = self.shown_at(&id);
            let between = if ghost < at { ghost + 1..at } else { at..ghost };
            if self.shown[between].iter().all(|(_, kind)| *kind != RowKind::Other) {
                self.shown[ghost] = (id, kind);
                out.push(VectorDiff::Set { index: ghost, value });
                return;
            }
            self.shown.remove(ghost);
            out.push(VectorDiff::Remove { index: ghost });
            if ghost < at {
                at -= 1;
            }
        }
        self.shown.insert(at, (id, kind));
        out.push(if push_back && at + 1 == self.shown.len() {
            VectorDiff::PushBack { value }
        } else {
            VectorDiff::Insert { index: at, value }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, Debug)]
    struct Row(&'static str, RowKind);

    impl OrderedRow for Row {
        fn row_id(&self) -> String {
            self.0.to_owned()
        }
        fn row_kind(&self) -> RowKind {
            self.1
        }
    }

    fn own(id: &'static str) -> Row {
        Row(id, RowKind::OwnRemote)
    }

    fn other(id: &'static str) -> Row {
        Row(id, RowKind::Other)
    }

    struct Pair {
        sdk: Vector<Row>,
        shown: Vector<Row>,
        order: SendOrder,
    }

    impl Pair {
        fn new(rows: Vec<Row>) -> Self {
            let sdk: Vector<Row> = rows.into_iter().collect();
            Self { order: SendOrder::new(&sdk), shown: sdk.clone(), sdk }
        }
        fn take(&mut self, batch: Vec<VectorDiff<Row>>) {
            for diff in batch.iter().cloned() {
                diff.apply(&mut self.sdk);
            }
            for diff in self.order.translate(batch) {
                diff.apply(&mut self.shown);
            }
        }
        fn shown(&self) -> Vec<&'static str> {
            self.shown.iter().map(|row| row.0).collect()
        }
        fn sdk(&self) -> Vec<&'static str> {
            self.sdk.iter().map(|row| row.0).collect()
        }
    }

    #[test]
    fn an_echo_refiled_behind_a_later_send_keeps_its_row() {
        let mut pair = Pair::new(vec![other("x"), own("one"), own("two")]);
        pair.take(vec![VectorDiff::Remove { index: 1 }, VectorDiff::PushBack { value: own("one") }]);
        assert_eq!(pair.sdk(), ["x", "two", "one"]);
        assert_eq!(pair.shown(), ["x", "one", "two"]);
        pair.take(vec![VectorDiff::Remove { index: 1 }, VectorDiff::PushBack { value: own("two") }]);
        assert_eq!(pair.shown(), ["x", "one", "two"]);
        pair.take(vec![
            VectorDiff::Set { index: 1, value: own("one") },
            VectorDiff::PushBack { value: other("y") },
        ]);
        assert_eq!(pair.shown(), pair.sdk());
    }

    #[test]
    fn rows_set_while_reordered_land_on_their_own_row() {
        let mut pair = Pair::new(vec![own("one"), own("two")]);
        pair.take(vec![VectorDiff::Remove { index: 0 }, VectorDiff::PushBack { value: own("one") }]);
        pair.take(vec![VectorDiff::Set { index: 0, value: own("two") }]);
        pair.take(vec![VectorDiff::PushBack { value: other("y") }]);
        assert_eq!(pair.sdk(), ["two", "one", "y"]);
        assert_eq!(pair.shown(), ["one", "two", "y"]);
        pair.take(vec![VectorDiff::Remove { index: 2 }]);
        assert_eq!(pair.shown(), ["one", "two"]);
    }

    #[test]
    fn an_echo_behind_someone_elses_event_follows_the_server() {
        let mut pair = Pair::new(vec![own("one")]);
        pair.take(vec![
            VectorDiff::Remove { index: 0 },
            VectorDiff::PushBack { value: other("x") },
            VectorDiff::PushBack { value: own("one") },
        ]);
        assert_eq!(pair.shown(), ["x", "one"]);
        assert_eq!(pair.shown(), pair.sdk());
    }

    #[test]
    fn an_own_event_removed_for_good_leaves() {
        let mut pair = Pair::new(vec![other("x"), own("one"), own("two")]);
        pair.take(vec![VectorDiff::Remove { index: 1 }]);
        assert_eq!(pair.shown(), ["x", "two"]);
        assert_eq!(pair.shown(), pair.sdk());
    }
}

#[cfg(test)]
mod sdk_tests {
    use super::*;
    use crate::timeline::lightning_event_filter;
    use futures_util::{Stream, StreamExt};
    use matrix_sdk::{
        ruma::{
            api::client::sync::sync_events::v5, event_id,
            events::room::message::RoomMessageEventContent, room_id, serde::Raw,
        },
        test_utils::mocks::MatrixMockServer,
        Client,
    };
    use matrix_sdk_base::RequestedRequiredStates;
    use matrix_sdk_ui::timeline::{TimelineBuilder, TimelineFocus};
    use serde_json::json;
    use std::time::{Duration, Instant};

    const OWN: &str = "@example:localhost";

    fn message(id: &str, sender: &str, body: &str) -> serde_json::Value {
        let ts = matrix_sdk::ruma::MilliSecondsSinceUnixEpoch::now().get();
        json!({"type": "m.room.message", "event_id": id, "sender": sender,
            "origin_server_ts": ts, "content": {"msgtype": "m.text", "body": body}})
    }

    async fn sync(client: &Client, events: Vec<serde_json::Value>, pos: &str) {
        let mut response = v5::Response::new(pos.to_owned());
        let mut room = v5::response::Room::default();
        room.timeline = events
            .into_iter()
            .map(|v| Raw::from_json_string(v.to_string()).unwrap())
            .collect();
        response.rooms.insert(room_id!("!order:localhost").to_owned(), room);
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .unwrap();
    }

    type Batch = Vec<VectorDiff<Arc<TimelineItem>>>;

    struct View {
        sdk: Vector<Arc<TimelineItem>>,
        shown: Vector<Arc<TimelineItem>>,
        order: SendOrder,
    }

    impl View {
        fn take(&mut self, batch: Batch) {
            for diff in batch.iter().cloned() {
                diff.apply(&mut self.sdk);
            }
            for diff in self.order.translate(batch) {
                diff.apply(&mut self.shown);
            }
        }
    }

    // Own message bodies in row order; a local echo is marked with `~`.
    fn own_bodies(rows: &Vector<Arc<TimelineItem>>) -> Vec<String> {
        rows.iter()
            .filter_map(|row| {
                let event = row.as_event()?;
                if !event.is_own() {
                    return None;
                }
                let body = event.content().as_message()?.body().to_owned();
                Some(if event.is_local_echo() { format!("{body}~") } else { body })
            })
            .collect()
    }

    fn ids(rows: &Vector<Arc<TimelineItem>>) -> Vec<String> {
        rows.iter().map(|row| row.unique_id().0.clone()).collect()
    }

    async fn until<S: Stream<Item = Batch> + Unpin>(
        view: &mut View,
        stream: &mut S,
        sdk_bodies: &[&str],
    ) {
        let deadline = Instant::now() + Duration::from_secs(10);
        while own_bodies(&view.sdk) != sdk_bodies {
            let left = deadline.saturating_duration_since(Instant::now());
            let batch = match tokio::time::timeout(left, stream.next()).await {
                Ok(Some(batch)) => batch,
                _ => panic!(
                    "SDK rows never became {sdk_bodies:?}: {:?}",
                    own_bodies(&view.sdk)
                ),
            };
            view.take(batch);
        }
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn sent_messages_stay_in_send_order_while_their_echoes_arrive() {
        let server = MatrixMockServer::new().await;
        let store = std::env::temp_dir().join(format!("lightning-send-order-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&store);
        let client = server
            .client_builder()
            .on_builder(|b| {
                b.sqlite_store(&store, None).with_threading_support(
                    matrix_sdk::ThreadingSupport::Enabled { with_subscriptions: false },
                )
            })
            .build()
            .await;
        client.event_cache().subscribe().unwrap();
        let room = server.sync_joined_room(&client, room_id!("!order:localhost")).await;
        server.mock_room_state_encryption().plain().mount().await;
        sync(&client, vec![message("$seed", "@alice:localhost", "seed")], "1").await;

        let timeline = TimelineBuilder::new(&room)
            .event_filter(lightning_event_filter)
            .with_focus(TimelineFocus::Live { hide_threaded_events: true })
            .build()
            .await
            .unwrap();
        let (items, stream) = timeline.subscribe().await;
        let mut stream = Box::pin(stream);
        let mut view = View { order: SendOrder::new(&items), sdk: items.clone(), shown: items };

        server.mock_room_send().ok(event_id!("$one")).up_to_n_times(1).mount().await;
        server.mock_room_send().ok(event_id!("$two")).mount().await;
        timeline.send(RoomMessageEventContent::text_plain("one").into()).await.unwrap();
        timeline.send(RoomMessageEventContent::text_plain("two").into()).await.unwrap();
        until(&mut view, &mut stream, &["one", "two"]).await;

        sync(&client, vec![message("$one", OWN, "one")], "2").await;
        until(&mut view, &mut stream, &["two", "one"]).await;
        assert_eq!(own_bodies(&view.shown), ["one", "two"], "display order after the first echo");

        sync(&client, vec![message("$two", OWN, "two")], "3").await;
        until(&mut view, &mut stream, &["one", "two"]).await;
        assert_eq!(own_bodies(&view.shown), ["one", "two"], "display order after both echoes");
        assert_eq!(ids(&view.shown), ids(&view.sdk));
    }
}
