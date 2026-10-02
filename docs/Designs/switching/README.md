# Physical switching and routing handoff

The Windows capture worker recognizes shortcuts from physical scan code plus
the E0 extended flag before guest mapping. Editable bindings are validated by
`HotkeyConfig::try_new`; duplicate trigger/modifier pairs and modifier-only
triggers are rejected. Defaults are Ctrl+Alt+F12 next, F11 previous, F10
local, and Ctrl+Alt+1/2/3 direct guests. Either physical modifier side counts.
The activating key down, repeat, and up are consumed. Modifier presses may
already have reached the old guest, so switching starts with `RELEASE_ALL`.
The both-Control one-second emergency is reserved and emits local return.

`CaptureService` starts disarmed but sends `PhysicalEvent::Hotkey` through its
bounded channel even in local mode. Its timer checks the emergency hold every
20 ms. An editable config can be installed with `set_hotkeys`; contention or
queue overflow faults disarm guest capture. Injected events are excluded from
shortcut recognition. The capture worker itself does not open USB or send
guest input.

`RequestActor` owns configured cycle order and firmware-reported readiness.
It includes local control in the cycle, skips offline guests, and accepts only
one switch transaction at a time. The latest non-local request waits behind a
transaction; local return preempts it. A direct offline selection fails local.
`Router` emits release, then selection after release ACK, then arm after
selection ACK. Firmware `RELEASE_ALL` advances its route generation: from a
confirmed generation 0, its ACK reports 1, `SWITCH` expects 1 and requests 2,
and `ARM` uses 2. `with_generation` seeds the actor from a confirmed nonzero
firmware STATUS after session negotiation. An arm ACK is required before a
guest becomes active. The actor
rejects stale generations, fails local on disconnect/timeout, and withholds
input until the caller confirms all physical keys and buttons were released.
It never queues pointer deltas for replay.

The native host I/O loop must consume capture events and issue these abstract
commands on a confirmed USB session. It must validate the session, sequence,
ACK kind and resulting firmware generation before calling the corresponding
actor ACK method; call `tick` before processing late ACKs. A mismatched
resulting generation requires local failover and session resynchronization.
It must disarm
capture on transition or fault, arm with the acknowledged generation only
after readiness and an all-up baseline, and call `observe_all_released` only
from a verified physical state ledger. The live host I/O loop and asynchronous
release ACK reconciliation are pending integration with the firmware contract. The
current source tests prove policy order, not delivery to a physical guest.

Verify on Windows 11 with a real keyboard and mouse: both Ctrl sides, left and
right Alt/Control, AltGr, repeat, trigger down/up leakage, editing during
capture, drag blocking, secure desktop boundaries, unplug during release,
firmware reset, and zero-state recovery. The board must pass its own CDC/HID
gates before claiming a complete end-to-end switch. These native checks are
unrun.
