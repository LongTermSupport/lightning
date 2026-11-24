`matrix-sdk-0.18.0-lightning/` is the crates.io `matrix-sdk` 0.18.0 package with
upstream commit 83e14bf13 ("The EventCache receives RoomUpdates from the sync
flow") backported: sync hands room updates to the event cache directly, so a
lagging listener can no longer clear every room's cached history.

One local change, not upstream ("known page after its gap"): in
`event_cache/caches/room/pagination.rs` a back-paginated page is dropped, and
its gap resolved, when every event in it is already stored after the gap being
filled, whoever sent them. Upstream keeps such a page when all of it is our
own and moves the events to the gap, which misorders the room when an older
gap is filled after a newer one already covered it. Events stored before the
gap, as the send queue's are, keep upstream's behaviour.

Remove it, and the `[patch.crates-io]` entry in `rust/Cargo.toml`, when a
matrix-sdk release includes that commit and the pagination change.
