# Routing fault suite

`crates/input-core/tests/faults.rs` is the executable suite. Run:

```powershell
cargo test -p esp32-kvm-input-core --test faults
```

The suite uses an explicit fake monotonic clock and a command recorder. It
checks the release/select/arm acknowledgment order, stale acknowledgments,
the exact 500 ms deadline in each phase, queued selection cancellation on
timeout/link loss, firmware reset or link loss, reconnect all-up baseline,
not-ready target, queue overflow, and suppression of held input until an
all-up observation **after** arming. These are source-level tests; they do not show
that the USB or BLE device delivered a release to a guest.
