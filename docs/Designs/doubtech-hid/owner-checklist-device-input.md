# Owner hardware checklist: device-owned keyboard and mouse

**NONE of this has been run.** The code behind it (`doubtech_hid_central`, `input_hub`, the
USB worker wiring and the touchscreen flow) is covered only by host tests with mocks and by
a successful firmware build. No radio, passkey, reconnect, chord, screen layout, heap or
latency behaviour has been observed on a board. Every item below is a PASS/FAIL line for the
owner to fill in; the agent that wrote it ran none of them.

Design: [central.md](central.md), [input hub](../input-hub/README.md),
[device screen state](../device-screens/screen-state.md).

## Before you start (the owner does this, never an agent)

1. **Flash the new firmware first.** Items a to q need the build from this branch on the
   board. Before that, quit the desktop app completely: tray icon, **Quit and release**. The
   app owns the board's serial port, and flashing needs it free. Do not stop it any other way.
2. Build and flash only with the script, never with another tool and **never erase flash**:

   ```powershell
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash -Port COM4
   ```

   `flash` writes the application image only (`idf.py app-flash`) and no erase option is
   ever passed, so the stored guest bonds (NVS) survive. Use `-Full` only if the partition
   table or bootloader changed, and it still never erases. The flash and port-list steps may
   reset the board and drop BLE links.
3. If `firmware/sdkconfig` already exists from an older build, delete that generated,
   git-ignored file once before the build so the new defaults apply (`MAX_CONNECTIONS=5`,
   `MAX_BONDS=12`, `BT_CTRL_BLE_MAX_ACT=8`, central role). Do not touch the board's flash for this.
4. Have ready: the keyboard and the mouse you want to use (note whether the keyboard asks for
   a passkey), three guests that are already paired with the board (and a fourth that is not),
   and a way to read the board's log for item j (see there). Keep the desktop app **closed**
   unless an item says otherwise.
5. Tell the person who gets the results which items were run and which were skipped. If an
   item fails, stop that item and capture what it lists; do not retry in a loop.

Notation: guest 1/2/3 are the slots shown on the touchscreen. The chord is Left Ctrl + Left
Alt + the key, typed on the board-owned keyboard.

## a. Pair the keyboard from the touchscreen

Steps:
1. With the app closed, tap the keyboard button beside the gear (bottom right) to open
   **INPUT DEVICES**; tap **ADD KEYBOARD OR MOUSE**.
2. Put the keyboard in pairing mode. Within 30 s its name appears as a row; tap it.
3. If the screen shows **TYPE ON KEYBOARD** and six digits, type them on the keyboard and
   press Enter.

Expected: the rows are listed strongest first (at most four). The result is **KEYBOARD PAIRED**
with its name, dismissed after about 4 s, and the list shows `KEYBOARD: <name>`. The footer
badge shows `KB`. Typed keys do nothing visible yet (target 0 is a stub) until item d.
A keyboard with no boot protocol must show **NOT SUPPORTED** instead.

Result: PASS / FAIL
Capture on FAIL: a photo of the screen at the failure, the exact title and hint text, the
keyboard make and model, whether it showed a passkey, the Health log lines (item j) and any
`doubtech_hid_central` log lines around the attempt.

## b. Pair the mouse

Steps: as item a for the mouse (**ADD KEYBOARD OR MOUSE**, tap its row).

Expected: **MOUSE PAIRED**, the list shows `MOUSE: <name>`, the badge shows `KB MS`. Moving it
does nothing visible until item d. A mouse the walker rejects shows **NOT SUPPORTED**.

Result: PASS / FAIL
Capture on FAIL: photo, the mouse make and model, and (if not supported) whether it is a
wheel mouse, a gaming mouse or has extra buttons; its Report Map if you can dump one.

## c. Power-cycle and automatic reconnect

Steps: with both paired, power-cycle the board (unplug and replug USB). Do not touch the screen.
Wait up to 60 s.

Expected: both devices reconnect on their own (badge `KB MS` returns without any tap), keyboard
first. If a device is off, it is retried about every 25 s, alternating; it connects when turned on.

Result: PASS / FAIL
Capture on FAIL: how long each took, which device was first, the badge over time, and the log.

## d. Select each guest from the chord and type and move the pointer

Steps: with the app closed and three guests connected, hold Left Ctrl + Left Alt and press 1
on the keyboard; type a word and move/click/scroll the mouse. Repeat with 2 and 3. Then
Left Ctrl + Left Alt + 0, then Left Ctrl + Left Alt + Esc.

Expected: keys and pointer go to the selected guest only, the chord keys themselves never
reach it, the touchscreen highlights the selected card, and nothing is stuck after a switch
(release the keys on the old guest). 0 and Esc select This PC: typing does nothing anywhere
(stub). A chord with Right Alt, or with another key held first, must not switch.

Result: PASS / FAIL (note guest 1 / 2 / 3 separately)
Capture on FAIL: which guest, which chord, what reached which guest, the card highlight, and
whether a retry (SWITCH_FAILED) is suspected.

## e. Keyboard off while a key is held

Steps: select a guest, hold a letter key, switch the keyboard off (or take its battery out).
Also hold Shift and a mouse button and turn the mouse off.

Expected: nothing stays pressed on the guest (no repeating letter, no stuck Shift or button).
The badge drops the `KB` / `MS` entry at once. The other device keeps working.

Result: PASS / FAIL
Capture on FAIL: guest OS, what stayed pressed and for how long, and the log.

## f. The PC app still wins while armed

Steps: open the desktop app, capture the PC keyboard and mouse and route them to a guest.
While armed, type on the board's own keyboard and move its mouse. Release the route in the app.

Expected: while the PC route is armed, the board's own keyboard and mouse do nothing (no
double input, no flicker). The PC capture reaches the guest. When released, the board's keyboard
and mouse work again on the hub target. The app never shows an error from arming (the hub never
answers BUSY).

Result: PASS / FAIL
Capture on FAIL: the app's route status, what reached the guest, and the log.

## g. Pair a fourth guest with both inputs bonded

Steps: with the keyboard and mouse paired (and stored), pair a fourth guest (or the first
free slot), through the touchscreen gear or the PC app, with numeric comparison.

Expected: guest pairing still works, shows the six-digit number, and bonds. The input devices
stay connected. Bond count: eight guests is still the limit (the ninth is refused).
Guest pairing during an input window must be refused (the screen shows **BUSY**).

Result: PASS / FAIL
Capture on FAIL: the pairing result text, the guest OS, and whether the inputs dropped.

## h. Ten minutes with the app closed

Steps: leave the board on with the app closed for 10 minutes, with at least one guest ready and
both inputs connected, then with no guest ready.

Expected: no flashing or flicker. The screen is the normal active screen; only the This PC
card is dimmed (50 percent) and the footer shows the `USB off | KB MS` badge. With no guest
ready, the same screen with "No guest connected". No pause screen from the missing PC app.

Result: PASS / FAIL
Capture on FAIL: a short video of the flicker, the badge text at the time, and the log.

## i. Guest notify latency while an input device is reconnecting (R3)

Steps: with a guest connected and the PC app armed to that guest (so the PC keyboard drives
it), hold a key on the PC keyboard for its autorepeat. On the guest run a page or tool that logs
key-event times (a web page logging `event.timeStamp` is enough). Record the intervals for 60 s
in each condition: (1) no input device stored; (2) a keyboard and mouse stored but switched off, so
reconnect attempts run every 25 s; (3) one input pairing window open (scanning).

Expected: repeat intervals stay about the same in all three; no gaps of more than about 100 ms
beyond the baseline. Record the maximum gap per condition in the table below.

| Condition | Max gap | p99 gap | Notes |
|---|---|---|---|
| 1 no inputs stored | | | |
| 2 reconnecting | | | |
| 3 scan window | | | |

Result: PASS / FAIL (state your threshold)
Capture on FAIL: the three interval logs, the guest and its OS, and the number of guests connected.

## j. Free heap and stack high-water marks

The firmware prints a line every 30 s from the USB worker (new in this change):

```text
I (...) esp32-kvm: Health: free=<bytes> min_free=<bytes> internal_min_free=<bytes> usb_worker_stack_min=<bytes> display_stack_min=<bytes>
```

`min_free` is the lowest free heap since boot. `usb_worker_stack_min` is the least free stack
the worker (6144 bytes) has had, and `display_stack_min` the least the display task (4096 bytes)
has had. They fall as code paths run, so read them after the busiest steps (items a, b, d, i).

Reading the log: the console is UART0, because USB Serial/JTAG is reserved for the PC
protocol (the build refuses to share it). Attach a 3.3 V USB-UART adapter to the board's UART0
pins (default GPIO43 TX and GPIO44 RX on the ESP32-S3; check your board's header) at 115200
baud and open a serial monitor. Do not open the board's own USB port: the PC app owns it, and
opening it can reset the board. If you have no adapter, mark this item NOT RUN.

Expected: after pairing both devices, three guests connected and a scan window opened once:
`min_free` and `internal_min_free` stay above 20000 bytes; `usb_worker_stack_min` and
`display_stack_min` stay above 512 bytes. Record the numbers:

| Moment | free | min_free | internal_min_free | usb_worker_stack_min | display_stack_min |
|---|---|---|---|---|---|
| Boot, idle | | | | | |
| 3 guests, 2 inputs connected | | | | | |
| After a scan window | | | | | |
| After the busiest run (d, i) | | | | | |

Result: PASS / FAIL / NOT RUN
Capture on FAIL: the full table, and the first Health line that fell below a threshold.
(The thresholds are suggestions, not measured limits; report the real numbers.)

## k. A freshly paired keyboard routes (the CONNECTED path)

Steps: forget the keyboard (item n), pair it again (item a), and **without** a power cycle select
a guest by chord and type.

Expected: the keyboard works at once after the PAIRED screen (the freshly paired link is the
same link that then reports CONNECTED; no reconnect or reboot is needed). The badge shows `KB`.

Result: PASS / FAIL
Capture on FAIL: the screen after PAIRED, whether `KB` showed, and the log.

## l. Keyboard or mouse that rotates its address (RPA)

Steps: if the keyboard or mouse uses a resolvable private address (many do; a phone's BLE
scanner such as nRF Connect shows the address changing over time), pair it, switch it off for
over 15 minutes (so its address rotates), then switch it on.

Expected: it reconnects on its own within about a minute, without being paired again.

Result: PASS / FAIL / NOT APPLICABLE (the device uses a static address)
Capture on FAIL: the device, whether the address rotated, how long it was off, and the log.
This is the unverified RPA reconnect from the known gaps.

## m. Scan airtime against guest notify latency

Steps: as item i condition 3, but with two guests actively typing/moving via the PC app and
the scan window open, plus a third guest connected and idle. Repeat three times.

Expected: no dropped or visibly delayed keys on the armed guest during the 30 s window; nothing
disconnects. The scan runs at half duty (60 ms interval, 30 ms window).

Result: PASS / FAIL
Capture on FAIL: the guest-side log, which guest dropped, and the scan result list at the time.

## n. Forget flow

Steps: open **INPUT DEVICES**, tap the `KEYBOARD: <name>` row once, then tap it again within 5 s.
Repeat for the mouse. Also tap a row once and wait more than 5 s. Also open the list while the
app has a route armed.

Expected: first tap shows `FORGET <name>?`, the row reads `TAP AGAIN TO FORGET`, the footer
`Tap again within 5s`. A second tap forgets it: **KEYBOARD FORGOTTEN** (or **MOUSE FORGOTTEN**),
the row reads `not paired`, the badge entry drops, held keys on the guest are released, and the
device no longer reconnects. After 5 s the confirmation lapses. With an armed PC route the list
shows **BUSY**. Guest bonds are never touched (check a guest still reconnects). The device can be
paired again.

Result: PASS / FAIL
Capture on FAIL: the screen texts, whether the other device and the guests were affected.

## o. Layout of the INPUT PAIR screens and the new keyboard button

Steps: look at every screen of the flow on the panel: the guest view with the keyboard button
(42 x 42, left of the gear) and the gear; the INPUT DEVICES list; the scan list with 0, 1 and 4
rows; the passkey screen; the PAIRED and failure screens (NOT SUPPORTED, TIMED OUT, PAIRING
FAILED, BUSY, NO FREE SLOT); the **Stopping pairing...** hold.

Expected: nothing is clipped, overlaps the close button or the footer, or is too small to touch;
the 28 pt passkey digits fit; the 18-character title clears the close button; the footer badge is
not truncated by the buttons; touch targets land where drawn (the layout has never been checked
on the panel).

Result: PASS / FAIL
Capture on FAIL: a photo of each screen that is wrong, with the row count, and the screen name.

## p. Keyboard sleep and wake

Steps: let the paired keyboard go to sleep (its idle timeout), then press a key.

Expected: the badge drops `KB` when it sleeps (nothing stays pressed), and it reconnects within
about a second or two of waking (one immediate retry per drop; otherwise within the shared
25 s schedule), after which typing reaches the selected guest again. A key that wakes the
keyboard may be lost; that is acceptable.

Result: PASS / FAIL
Capture on FAIL: the keyboard model, how long it took to return, whether it returned at all,
and the log.

## q. Mouse lost while typing

Steps: select a guest, type continuously on the keyboard, then switch the mouse off in the middle.

Expected: typing is not interrupted and no key is released early. The mouse buttons it held are
released (item e); the chord and typing still work. When the mouse is switched on it reconnects
and moves the pointer again.

Result: PASS / FAIL
Capture on FAIL: what stopped, for how long, and the log.

## Summary line to return

`a __ b __ c __ d1 __ d2 __ d3 __ e __ f __ g __ h __ i __ j __ k __ l __ m __ n __ o __ p __ q __`
(PASS, FAIL or NOT RUN for each), the firmware build you flashed (its `esp32_kvm.bin` size and
date), and the captures for every FAIL.
