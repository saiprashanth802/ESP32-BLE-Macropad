# Dial Satellite — Engineering Build Log

Dated record of every bench session on the dial satellite: what was tried,
what the instruments actually said, what failed and why. Design and frozen
decisions live in `DIAL_SATELLITE.md`; this file is the history.

Rule for this log: **record measurements, not impressions.** If a claim has no
observation behind it, it does not belong here.

---

## 2026-07-27 — Session 1: ESP-12E bring-up (BLOCKED)

**Goal:** §7.1 step 0 — flash `firmware/tests/esp12e_bringup.ino` onto a bare
ESP-12E and confirm a steady heartbeat.

**Outcome: blocked.** No firmware was flashed. Blocked on the USB-serial path,
not on the module. Full detail below because the failure was expensive and the
diagnosis is worth not repeating.

### Hardware configuration

- Bare ESP-12E module (2 mm pitch, hand-wired — no adapter PCB).
- Boot-pin biasing per `CONTEXT.md` §2: EN/RST/GPIO0/GPIO2 pulled up 10 kΩ,
  GPIO15 pulled down 10 kΩ.
- Powered from an ESP32 dev board's 3V3 rail (shared ground).
- Serial attempted via two adapters over the session — see below.

### What was established (evidence-backed)

| Finding | Evidence |
|---|---|
| ESP-12E is powered, resets, and boots | Onboard LED (GPIO2 = UART1 TX) flickers on every RST tap. |
| The module reached download mode at least once | An early `esptool chip_id` succeeded, before the wiring was disturbed. |
| **The PL2303 adapter is dead or counterfeit** | `bcdDevice 3.00` (PL2303HXA, discontinued 2012, heavily counterfeited). Driver binds and the port enumerates, but a loopback with its own TX shorted to its own RX returned **0 bytes on 5/5 trials**. |
| The CP2102 adapter is functional | Received a continuous stream from the attached ESP32 all session; went to **exactly 0 bytes** once the ESP32's EN was jumpered to GND. |
| ESP-12E TX never reached any adapter RX | 25 s capture at **74880 baud** (native ROM rate) across multiple RST taps: **0 bytes**. Silence, not garbage — which rules out a baud mismatch, since a wrong baud yields corrupt bytes rather than none. |

### The expensive mistake — port identity

Two USB-serial adapters were connected simultaneously:

- `/dev/ttyUSB0` — PL2303 (Prolific)
- `/dev/ttyUSB1` — CP2102 (Silicon Labs)

Which adapter served which board was misidentified **twice**, and several rounds
of rewiring were spent probing the ESP32's port while believing it was the
ESP-12E's. The ESP32 was running firmware and streaming continuously, so its
port produced endless deterministic garbage that was repeatedly misread as a
signal from the ESP-12E.

Compounding it, the ports **renumbered mid-session** when the PL2303 was
unplugged — the CP2102 moved from `ttyUSB1` to `ttyUSB0`.

**Rules adopted for future sessions:**

1. **Connect one adapter at a time.** If two must be present, identify them by
   unplugging one and observing which node disappears — before any probing.
2. **Re-check the port node after any replug.** Numbering is not stable.
3. Identify by `udevadm info -q property -n <dev> | grep ID_MODEL` and record
   the mapping in this log at the start of the session.

### Diagnostic technique worth keeping

- **Silence vs. garbage is the key discriminator.** A wrong baud rate produces
  corrupt bytes; a disconnected line produces nothing. `0 bytes` at 74880 with
  a confirmed-booting chip is proof of an open TX path, not a config error.
- **Reading the ROM banner at 74880** identifies boot mode directly
  (`boot mode:(1,x)` = UART download, `(3,x)` = flash boot). `stty` cannot set
  74880; `scratchpad/read74880.py` does it via the `TCSETS2` ioctl with
  `BOTHER`. Note the PL2303 cannot do 74880 at all; the CP2102 can.
- **Loopback with the adapter's own TX shorted to its own RX** is the cleanest
  bisect available — it tests the adapter, driver and cabling with the target
  removed entirely. Both adapters were tested this way. It is the test that
  condemned the PL2303.
- **An ESP32 dev board can serve as a USB-serial bridge** by jumpering `EN` to
  `GND`, which parks the ESP32 and frees its adapter. Verified working: the
  line went from flooded to completely silent. Non-destructive and reversible —
  nothing is flashed or erased.

### Unresolved at session end

The ESP-12E's TX (GPIO1) does not reach any adapter RX. Remaining candidates,
none yet distinguished:

- Cold or open solder joint on the module's TX pad (2 mm castellations lift
  easily; a joint can look sound while sitting on lifted copper).
- Wrong pad on the module.
- The ESP32 board's `TX0`/`RX0` header pins not being the CP2102's data lines —
  a loopback between those two header pins also returned 0 bytes, despite the
  CP2102 being known-good.

**Software probing is exhausted here.** Every remaining hypothesis is about
whether one piece of copper connects to another, which only a continuity meter
can answer. No further rewiring should be attempted blind.

### Next session — do these in order

1. **Continuity-check with a multimeter**: ESP-12E TX pad → adapter RXD pin,
   and ESP-12E RX pad → adapter TXD pin. This is the measurement that would
   have replaced most of this session.
2. **Use a purpose-built USB-TTL dongle** (genuine CH340 or CP2102). The
   ESP32-as-bridge route works but adds an `EN` jumper and header-pin ambiguity
   to a system that already had too many unknowns.
3. Only then retry §7.1 step 0.

### Cost

One full session, no firmware flashed. Root cause of the *lost time* was not
the hardware fault itself but **debugging two unknowns at once** — an
unverified serial adapter and an unverified module — with an unstable port
mapping between them. Verify the adapter alone, first, next time.

---
