# Input hub: device-owned keyboard and mouse routing

`firmware/components/input_hub` is the pure C routing core for the keyboard and mouse that
the board owns (paired through [doubtech_hid_central](../doubtech-hid/central.md)). It
holds no platform state, uses no NimBLE, and its caller serializes all calls with the
router. It is firmware-internal and shares the router's `kvm_*` types, so unlike
`doubtech_hid` and `doubtech_hid_central` it is **not** covered by the product-name guard.

The glue that runs it lives in `firmware/components/transport` (`transport_hub.[ch]`, the
worker in `transport_usb_serial_jtag.c`, the `kvm_router_hid_central_msg` conversion in
`router_hid_bridge.c`). Status: host-tested with mocks; **not run on a board**. The owner
checklist is [owner-checklist-device-input.md](../doubtech-hid/owner-checklist-device-input.md).

## Where it runs

```text
NimBLE host thread:  central on_input / on_event  -> copy, push to a queue of 16, return
USB worker thread:   kvm_hub_worker_iteration     -> host bytes first, then the tick,
                                                      then the device drain (<= 16 messages,
                                                      <= 20 ms, stops at the first failure)
```

All router and hub state is touched only on the USB worker. Queue policy (depth 16): a new
pure-motion report is dropped when the ring is full; anything else evicts the oldest queued
motion; with nothing to evict the queue keeps only the newest state per kind (keyboard,
consumer, pointer) so the final key or button release always arrives. A device-lost
marker is a flag outside the ring (one per kind), delivered in order and never dropped.
While the queue holds anything the USB read timeout is 2 ms instead of 20 ms.

## Targets and the hub target

Target 0 is **This PC** (a stub sink: nothing is emitted; typing at it does nothing until
stage 3), targets 1 to 3 are the guest slots. The hub target is the hub's own selection; it
is **independent of `router.slot`** (the slot the PC app selected). The hub's output goes
to the same guest channels the router uses, through a shim.

## Arbitration

| Situation | What happens to device input |
|---|---|
| The router is armed (the PC app is driving a guest) | Dropped. The hub clears its local state and **emits nothing**; the host route always wins. Loss reports emit nothing either. |
| A host session is open, not armed (PC app connected, idle, or on This PC) | Goes to the hub target. An open session does **not** gate device input. |
| No host session | Goes to the hub target. |
| The host sends ARM | **Hub yield before arm**: inside the router's output `arm` callback (it runs only after the router has accepted the ARM: session, lease, sequence, generation, slot and ready checks all passed) the hub sends all-up to the slot it holds and releases it, on any slot, not only the armed one. If that fails, the worker releases the hub's channel directly, then disconnects the slot if that is also uncertain, and the router's arm proceeds. The host never receives BUSY because of the hub; the worst case is the router's existing NOT_READY for a slot that had to be disconnected. A stale, replayed or wrong-slot ARM never reaches the hub. |
| The host releases or disconnects a slot the hub holds | The hub abandons its arm (the channel release already sent all-up). |

While the router is not armed, the hub arms its target lazily on the first forwarded report,
and never arms a slot the router holds. Held state is cleared per report only when the send
succeeds. A slot that is not ready cannot hold keys, so its held state is abandoned, not
retried; a failure on a ready slot keeps state and is retried. A guest DISCONNECTED event
for the hub's slot abandons the arm; an unknown slot releases properly. The emergency BOOT
release and a pairing-queue overflow also release the hub.

## Chords

All chords are typed on the board-owned keyboard. The chord rule is **stricter than "the
first non-modifier key"**:

- The modifier byte must be exactly Left Ctrl + Left Alt (`0x05`): extra modifiers
  (including Right Ctrl or Right Alt) do not make a chord.
- Exactly **one** non-modifier key must be **newly pressed**, with no other non-modifier
  key held before it (the hub tracks the raw previous report), so rollover and keys held
  across a modifier press do not fire.

| Keys | Action |
|---|---|
| Left Ctrl + Left Alt + `1` / `2` / `3` | Select guest 1 / 2 / 3 |
| Left Ctrl + Left Alt + `0` | Select This PC (target 0, the stub) |
| Left Ctrl + Left Alt + `Esc` | Release all: all-up on the old target and target 0 |

Behaviour:

- **Swallow**: the chord and everything after it is swallowed until every non-modifier key
  is up (the all-keys-up report itself is swallowed). The old target gets all-up and its
  arm is released before the new target is armed. A chord to the already selected target
  still yields (all-up of held modifiers, drop the arm) while swallowing the key.
- **Latch**: while latched, keyboard reports are not forwarded; a fresh chord clears a stale
  pending switch.
- **SWITCH_FAILED**: when releasing a ready old target fails, the result is
  `KVM_HUB_SWITCH_FAILED` and the old target is kept. The switch is retried once per later
  swallowed report; the all-keys-up report makes a final retry and then abandons the
  switch (held state and arm are kept), so a new chord always works. A pending switch
  never kills the keyboard. The worker counts failures. A not-ready old target never
  blocks a switch.
- A touchscreen card tap and PLUS also set the hub target when no host session is open
  (PLUS steps 0, then the ready guests, then 0); with a host session they keep the old
  behaviour (a request to the host). A touch selection whose release fails stays pending and
  is retried before each later device report. A chord result cancels a pending touch selection.
- After a chord the Ctrl+Alt release can reach the new target as a modifier-up without a
  prior press (harmless).

## Losing a device

The central emits a `lost` input when a device that delivered input disconnects (power-off,
out of range, forget, abort). The hub then releases only what that kind could hold
(`kvm_input_hub_device_lost_kind`): a keyboard loss sends all-up keyboard plus zero
consumer when held and resets chord tracking; a mouse loss sends zero pointer state, so the
other device keeps working. The arm is released when nothing was held. While the router is
armed it only abandons local state. A not-ready or disconnected target drops events and
keeps the hub target.

## Display

With no PC app session (USB off) the screen is the same active screen, not a pause. The only
differences are that the This PC card is dimmed to 50 percent and the footer shows a badge
`USB off` plus `KB` and `MS` for the connected inputs. The highlighted card is the hub
target when no host route is armed. See
[device screen state](../device-screens/screen-state.md).

## Tests

`firmware/components/input_hub/tests/run-msvc.ps1` (the pure hub: chords, latch, retry,
yield, loss kinds) and `firmware/tests/integration/run-msvc.ps1` (`test_hub_worker`: queue
policy, arbitration, yield before arm through real host frames, drain bounds, input
pairing gates). Neither runs on a board. They are not in CI.

## Limits

- This PC is a stub until stage 3.
- The worker stack is 8192 bytes (it runs the router, the hub, the central bridge and the
  central start, which does an NVS load); not measured. The 30 s `Health:` log line reports
  its high-water mark beside the display and NimBLE host tasks (owner checklist item j).
- A slot that stays ready but keeps failing keeps a switch pending (retried on later
  swallowed reports); `Esc` or the slot's loss ends it.
