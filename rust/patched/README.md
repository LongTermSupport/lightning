`matrix-sdk-0.18.0-lightning/` is the crates.io `matrix-sdk` 0.18.0 package with
upstream commit 83e14bf13 ("The EventCache receives RoomUpdates from the sync
flow") backported: sync hands room updates to the event cache directly, so a
lagging listener can no longer clear every room's cached history.

One local change, not upstream: in `event_cache/caches/room/pagination.rs` a
back-paginated page whose events are all already cached is dropped whoever sent
them. Upstream keeps a page made only of our own events and moves those events
to the gap being filled, which misorders the room when an older gap is filled
after a newer one already covered it.

Remove it, and the `[patch.crates-io]` entry in `rust/Cargo.toml`, when a
matrix-sdk release includes that commit and the pagination change.
