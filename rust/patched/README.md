`matrix-sdk-0.18.0-lightning/` is the crates.io `matrix-sdk` 0.18.0 package with
upstream commit 83e14bf13 ("The EventCache receives RoomUpdates from the sync
flow") backported: sync hands room updates to the event cache directly, so a
lagging listener can no longer clear every room's cached history.

Remove it, and the `[patch.crates-io]` entry in `rust/Cargo.toml`, when a
matrix-sdk release includes that commit.
