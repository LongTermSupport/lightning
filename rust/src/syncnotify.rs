
use std::{
    collections::{HashMap, VecDeque},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Mutex, OnceLock,
    },
    time::Instant,
};

use matrix_sdk::ruma::{EventId, OwnedEventId, OwnedRoomId, RoomId};

pub(crate) const AGE_ALLOWANCE_MS: u64 = 10_000;

pub(crate) const TIMESTAMP_ALLOWANCE_MS: u64 = 120_000;

/// Milliseconds on a clock that never steps, for the catch-up floor.
pub(crate) fn mono_ms() -> u64 {
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    ORIGIN.get_or_init(Instant::now).elapsed().as_millis() as u64
}

#[derive(Clone, Copy)]
pub(crate) struct Now {
    pub(crate) wall_ms: u64,
    pub(crate) mono_ms: u64,
}

pub(crate) fn now() -> Now {
    Now { wall_ms: crate::push_verdict::now_ms(), mono_ms: mono_ms() }
}

/// Whether a sync-handler event is history. One per bridge, driven by the sync
/// loop:
///
///  * `begin` when a sync session starts: everything from before it is old.
///  * `connection_lost` on any edge out of Running (offline, error, a
///    recovering room list): the next response is a catch-up, and an event in
///    it is new only if it reached the server within the allowance.
///  * `running_edge` on the edge back into Running: from now on an event is
///    new if it reached the server after this moment. Room-list growth after
///    it (rooms 100+, a newly subscribed warm room's 20-event history) only
///    ever re-delivers events from before it.
///
/// Monotonic floor, so nothing an earlier edge called history becomes news.
/// It is kept on `mono_ms`, so a wall-clock step cannot move it against
/// `unsigned.age`; only the timestamp fallback reads the wall clock, rebased
/// on the current one.
pub(crate) struct CatchupClock {
    floor_ms: AtomicU64,
    catching_up: AtomicBool,
}

impl Default for CatchupClock {
    fn default() -> Self {
        Self { floor_ms: AtomicU64::new(0), catching_up: AtomicBool::new(true) }
    }
}

impl CatchupClock {
    pub(crate) fn begin(&self, mono_ms: u64) {
        self.floor_ms.store(mono_ms, Ordering::SeqCst);
        self.catching_up.store(true, Ordering::SeqCst);
    }

    pub(crate) fn connection_lost(&self) {
        self.catching_up.store(true, Ordering::SeqCst);
    }

    pub(crate) fn running_edge(&self, mono_ms: u64) {
        self.floor_ms.fetch_max(mono_ms, Ordering::SeqCst);
        self.catching_up.store(false, Ordering::SeqCst);
    }

    /// `timestamp_ms`: `origin_server_ts`. `age_ms`: `unsigned.age` as the
    /// server sent it, which needs no clock agreement and so decides when
    /// present.
    pub(crate) fn is_backlog(&self, timestamp_ms: u64, age_ms: Option<u64>, now: Now) -> bool {
        let floor = self.floor_ms.load(Ordering::SeqCst);
        let limit = if self.catching_up.load(Ordering::SeqCst) {
            floor.max(now.mono_ms)
        } else {
            floor
        };
        match age_ms {
            Some(age) => {
                now.mono_ms.saturating_add(AGE_ALLOWANCE_MS) < limit.saturating_add(age)
            }
            None => {
                let limit_wall = now.wall_ms.saturating_sub(now.mono_ms.saturating_sub(limit));
                timestamp_ms.saturating_add(TIMESTAMP_ALLOWANCE_MS) < limit_wall
            }
        }
    }
}

pub(crate) const MAX_PENDING_UTDS: usize = 2_048;

#[derive(Default)]
pub(crate) struct PendingUtds {
    entries: HashMap<(OwnedRoomId, OwnedEventId), bool>,
    order: VecDeque<(OwnedRoomId, OwnedEventId)>,
}

impl PendingUtds {
    pub(crate) fn remember(&mut self, room_id: &RoomId, event_id: &EventId, backlog: bool) {
        let key = (room_id.to_owned(), event_id.to_owned());
        if self.entries.insert(key.clone(), backlog).is_some() {
            return;
        }
        self.order.push_back(key);
        while self.order.len() > MAX_PENDING_UTDS {
            if let Some(oldest) = self.order.pop_front() {
                self.entries.remove(&oldest);
            }
        }
    }

    pub(crate) fn take(&mut self, room_id: &RoomId, event_id: &EventId) -> Option<bool> {
        let key = (room_id.to_owned(), event_id.to_owned());
        let backlog = self.entries.remove(&key)?;
        self.order.retain(|entry| entry != &key);
        Some(backlog)
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }
}

#[derive(Default)]
pub(crate) struct SyncNotify {
    pub(crate) catchup: CatchupClock,
    pending_utds: Mutex<PendingUtds>,
}

impl SyncNotify {
    pub(crate) fn remember_utd(&self, room_id: &RoomId, event_id: &EventId, backlog: bool) {
        if let Ok(mut pending) = self.pending_utds.lock() {
            pending.remember(room_id, event_id, backlog);
        }
    }

    pub(crate) fn take_utd(&self, room_id: &RoomId, event_id: &EventId) -> Option<bool> {
        self.pending_utds.lock().ok()?.take(room_id, event_id)
    }

    pub(crate) fn reset(&self) {
        if let Ok(mut pending) = self.pending_utds.lock() {
            *pending = PendingUtds::default();
        }
        self.catchup.begin(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::{event_id, room_id};

    const MIN: u64 = 60_000;
    const HOUR: u64 = 60 * MIN;

    fn at(ms: u64) -> Now {
        Now { wall_ms: ms, mono_ms: ms }
    }

    #[test]
    fn an_old_event_after_the_first_running_edge_is_backlog() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start + 5_000);
        let month = 30 * 24 * HOUR;
        assert!(clock.is_backlog(start - month, Some(month), at(start + 20_000)));
        assert!(clock.is_backlog(start - month, None, at(start + 20_000)));
    }

    #[test]
    fn a_live_event_after_the_edge_is_not_backlog() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start + 5_000);
        let now = start + 10 * MIN;
        assert!(!clock.is_backlog(now - 300, Some(300), at(now)));
        assert!(!clock.is_backlog(now - 300, None, at(now)));
    }

    #[test]
    fn a_catch_up_response_after_a_lost_connection_is_backlog_except_the_newest_seconds() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start + 5_000);
        clock.connection_lost();
        let wake = start + 8 * HOUR;
        assert!(clock.is_backlog(wake - 5 * HOUR, Some(5 * HOUR), at(wake)));
        assert!(!clock.is_backlog(wake - 2_000, Some(2_000), at(wake)));
        clock.running_edge(wake + 1_000);
        assert!(clock.is_backlog(wake - HOUR, Some(HOUR + 30_000), at(wake + 30_000)));
        assert!(!clock.is_backlog(wake + 20_000, Some(1_000), at(wake + 21_000)));
    }

    #[test]
    fn the_age_decides_over_a_skewed_timestamp() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start);
        let now = start + HOUR;
        assert!(!clock.is_backlog(now - HOUR, Some(500), at(now)));
    }

    #[test]
    fn a_timestamp_within_the_skew_allowance_of_the_edge_is_live() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start);
        assert!(!clock.is_backlog(start - MIN, None, at(start + 1_000)));
    }

    #[test]
    fn the_floor_never_moves_back() {
        let clock = CatchupClock::default();
        clock.begin(10 * HOUR);
        clock.running_edge(10 * HOUR);
        clock.running_edge(9 * HOUR);
        assert!(clock.is_backlog(9 * HOUR + MIN, Some(HOUR - MIN), at(10 * HOUR + 1)));
    }

    fn stepped(wall_ms: u64, mono_ms: u64) -> Now {
        Now { wall_ms, mono_ms }
    }

    #[test]
    fn a_backward_wall_clock_step_does_not_make_a_live_event_backlog() {
        let clock = CatchupClock::default();
        let wall = 1_000 * HOUR;
        let mono = 7 * MIN;
        clock.begin(mono);
        clock.running_edge(mono + 5_000);
        let now = stepped(wall + MIN - 5 * MIN, mono + MIN);
        assert!(!clock.is_backlog(now.wall_ms - 100, Some(100), now));
        assert!(!clock.is_backlog(now.wall_ms - 100, None, now));
        assert!(clock.is_backlog(wall - HOUR, Some(HOUR), now));
        assert!(clock.is_backlog(wall - 5 * MIN - HOUR, None, now));
    }

    #[test]
    fn a_forward_wall_clock_step_does_not_replay_history() {
        let clock = CatchupClock::default();
        let wall = 1_000 * HOUR;
        let mono = 7 * MIN;
        clock.begin(mono);
        clock.running_edge(mono + 5_000);
        let now = stepped(wall + MIN + HOUR, mono + MIN);
        assert!(clock.is_backlog(wall - 10 * MIN, Some(11 * MIN), now));
        assert!(clock.is_backlog(wall - 10 * MIN, None, now));
        assert!(!clock.is_backlog(now.wall_ms - 2_000, Some(2_000), now));
    }

    #[test]
    fn what_reached_the_server_during_a_suspend_is_backlog_after_it() {
        let clock = CatchupClock::default();
        let wall = 1_000 * HOUR;
        let mono = 7 * MIN;
        clock.begin(mono);
        clock.running_edge(mono + 5_000);
        clock.connection_lost();
        let wake = stepped(wall + 8 * HOUR, mono + MIN);
        assert!(clock.is_backlog(wake.wall_ms - 5 * HOUR, Some(5 * HOUR), wake));
        assert!(clock.is_backlog(wake.wall_ms - 5 * HOUR, None, wake));
        assert!(!clock.is_backlog(wake.wall_ms - 2_000, Some(2_000), wake));
        clock.running_edge(wake.mono_ms + 1_000);
        let later = stepped(wake.wall_ms + 30_000, wake.mono_ms + 30_000);
        assert!(clock.is_backlog(wake.wall_ms - HOUR, Some(HOUR + 30_000), later));
        assert!(!clock.is_backlog(later.wall_ms - 500, Some(500), later));
    }

    #[test]
    fn a_young_process_still_calls_an_old_event_backlog() {
        let clock = CatchupClock::default();
        clock.begin(0);
        clock.running_edge(2_000);
        let now = stepped(1_000 * HOUR, 3_000);
        assert!(clock.is_backlog(now.wall_ms - HOUR, Some(HOUR), now));
        assert!(clock.is_backlog(now.wall_ms - HOUR, Some(u64::MAX), now));
        assert!(!clock.is_backlog(now.wall_ms - 100, Some(100), now));
    }

    #[test]
    fn a_pending_utd_is_taken_once_with_its_backlog_mark() {
        let notify = SyncNotify::default();
        let room = room_id!("!r:example.org");
        notify.remember_utd(room, event_id!("$a:example.org"), false);
        notify.remember_utd(room, event_id!("$b:example.org"), true);
        assert_eq!(notify.take_utd(room, event_id!("$a:example.org")), Some(false));
        assert_eq!(notify.take_utd(room, event_id!("$a:example.org")), None, "once");
        assert_eq!(notify.take_utd(room, event_id!("$b:example.org")), Some(true));
        assert_eq!(notify.take_utd(room, event_id!("$c:example.org")), None);
        notify.remember_utd(room, event_id!("$d:example.org"), false);
        assert_eq!(
            notify.take_utd(room_id!("!other:example.org"), event_id!("$d:example.org")),
            None
        );
    }

    #[test]
    fn pending_utds_are_bounded_oldest_first_and_reset_clears_them() {
        let mut pending = PendingUtds::default();
        let room = room_id!("!r:example.org");
        for i in 0..(MAX_PENDING_UTDS + 10) {
            let id = EventId::parse(format!("$e{i}:example.org")).unwrap();
            pending.remember(room, &id, false);
        }
        assert_eq!(pending.len(), MAX_PENDING_UTDS);
        assert_eq!(pending.take(room, event_id!("$e0:example.org")), None);
        let newest = EventId::parse(format!("$e{}:example.org", MAX_PENDING_UTDS + 9)).unwrap();
        assert_eq!(pending.take(room, &newest), Some(false));

        let notify = SyncNotify::default();
        notify.remember_utd(room, event_id!("$x:example.org"), false);
        notify.reset();
        assert_eq!(notify.take_utd(room, event_id!("$x:example.org")), None);
    }
}
