# MacroPad v5 — Config & OTA API

The device exposes a small HTTP + JSON API used to change layouts, key
assignments, shortcuts and to push firmware updates over the air. This is the
stable contract a companion app or script targets.

## Entering Config Mode

On the device: **Settings → CONFIG (WiFi/OTA)**. This:

1. Tears down BLE (frees the radio + RAM — WiFi and BLE don't coexist well on
   the WROOM-32).
2. Starts a WiFi SoftAP:
   - SSID: `MacroPad-Setup`
   - Password: `macropad123`
   - Device IP: `http://192.168.4.1`
3. Serves a built-in web UI at `/` and the JSON API below.

Exit: tap **Exit** in the web UI (`POST /api/exit`) or **hold FN (K9)** on the
device for ~1.5 s. Either way the device reboots back into keyboard mode.

> While in Config Mode the macropad is **not** a keyboard — it's a WiFi access
> point. This is by design.

## Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET  | `/`             | Built-in web config UI (HTML) |
| GET  | `/api/info`     | Device/firmware/capability info |
| GET  | `/api/actions`  | Built-in action library (id + label) |
| GET  | `/api/config`   | Full config (settings + presets + face) |
| POST | `/api/config`   | Apply + persist config (JSON body) |
| POST | `/api/update`   | OTA firmware upload (multipart `.bin`) |
| POST | `/api/exit`     | Save nothing, reboot to keyboard mode |
| GET  | `/api/anim`        | List uploaded GIF animations + storage usage |
| POST | `/api/anim`        | Upload a `.gif` (multipart; ≤ ~700 KB free space) |
| POST | `/api/anim/select` | `{"name":"/x.gif"}` — set as active face animation |
| POST | `/api/anim/delete` | `{"name":"/x.gif"}` — delete from storage |

### `GET /api/info`
```json
{ "device":"ESP32 MacroPad", "fw":"v5-multihost", "api":3,
  "keys":12, "presets":8, "slots":3, "macroMax":6, "heap":123456 }
```

### `GET /api/config` / `POST /api/config`
```json
{
  "api": 2,
  "settings": { "brightness":180, "sleepMin":5, "linux":false, "activePreset":0 },
  "presets": [
    { "name":"ONSHAPE", "keys": [ /* exactly 12 key objects */ ] }
  ]
}
```
POST may send a partial document — only the `settings` fields and `presets`
entries present are applied; the rest are left unchanged. Presets and keys are
matched positionally (preset 0..7, key 0..11). Saved to NVS immediately.

### Key object — one of five `type`s

```jsonc
// built-in action from /api/actions (OS-layout aware)
{ "label":"Fit", "type":"builtin", "id":140 }

// arbitrary key chord: mod is a bitmask Ctrl=1 Shift=2 Alt=4 Gui=8
{ "label":"SaveAs", "type":"key", "mod":3, "key":22 }         // Ctrl+Shift+S

// media / consumer usage code
{ "label":"Play", "type":"consumer", "consumer":205 }

// type an ASCII string (max 23 chars)
{ "label":"email", "type":"text", "text":"me@example.com" }

// macro: up to 6 steps, each a chord OR a consumer, with a post delay (×10ms)
{ "label":"Run", "type":"macro", "steps":[
    { "mod":8, "key":21, "delay":30 },   // Gui+R, wait 300ms
    { "mod":0, "key":40 }                // Enter
]}

// host: notify the companion app over the host-link GATT service instead of
// typing. mod/key is an OPTIONAL fallback chord sent when no app is listening.
{ "label":"Steam", "type":"host", "mod":0, "key":0 }
```

- `mod` bitmask: `Ctrl=1 Shift=2 Alt=4 Gui(Win/Cmd)=8` (combine by OR).
- `key` is a **USB HID keyboard usage code** (decimal). e.g. A=4 … Z=29,
  1=30 … 0=39, Enter=40, Tab=43, F11=68, arrows Right/Left/Down/Up=79/80/81/82.
- `consumer` is a **USB HID consumer usage code**: Play/Pause=205, Next=181,
  Prev=182, Mute=226, Vol+=233, Vol−=234.
- An **empty** slot is `{ "label":"", "type":"builtin", "id":0 }` (id 0 = A_NONE).

### `POST /api/update` — OTA
Multipart/form-data with the compiled `firmware.bin` as the file part. On
success the device flashes the inactive OTA partition and reboots into it; on
failure the running firmware is untouched. Build the `.bin` with the exact
board options below or the device won't boot it.

## Face / screensaver ("expression packs")

The device shows a state-reactive robot-eyes face (blinks when idle-connected,
darts when disconnected, widens in pairing, winks toward a slot on Easy-Switch,
droops before sleep) or an uploaded looping GIF. Everything is controlled from
the `face` object in `/api/config`:

```jsonc
"face": {
  "mode":  "idle",        // "off" | "idle" (screensaver) | "always"
  "style": "eyes",        // "eyes" (procedural) | "gif" (uploaded loop)
  "gif":   "/idle.gif",   // active animation (from /api/anim)
  "personality": "calm",  // "calm" | "playful" | "grumpy" | "sleepy"
  "eyes": {               // the EXPRESSION PACK — fully generative
    "color": 0,           // RGB565 eye color; 0 = follow active preset color
    "eyeW": 64, "eyeH": 84, "gap": 44, "round": 18,
    "blinkMinS": 3,  "blinkMaxS": 6,      // idle blink interval (s)
    "glanceMinS": 7, "glanceMaxS": 15,    // idle glance interval (s)
    "pairScalePct": 115,                  // wide-eye scale in pairing mode
    "idleS": 12                           // IDLE: seconds before the face shows
  }
}
```

All eye fields are clamped device-side to renderable bounds. A companion app
"generates a personality" by POSTing a new `eyes` object — no reflash needed.

### Personality

`eyes` sets how the face *looks*; `personality` sets how it *behaves*. Each
persona is a tuning table baked into firmware that scales blink and glance
intervals, emote intensity, micro-behaviour rates, and the resting posture of
the lids:

| Persona   | Feel                                                        |
|-----------|-------------------------------------------------------------|
| `calm`    | Steady, slightly heavy lids, unhurried glances (default)     |
| `playful` | Fast blinks, frequent glances and saccades, big reactions    |
| `grumpy`  | Slow, inward-slanted lids, damped reactions                  |
| `sleepy`  | Half-lidded, rare glances, yawns often                       |

POST accepts the name (case-insensitive) or the index `0..3`. Unknown names are
ignored rather than clamped, so a typo can't silently change the persona.

On top of the persona, a **mood** drifts on a minutes-long clock: energy tracks
how much you have been typing, valence tracks whether the BLE link is healthy.
Mood biases lid weight and lid angle, so a busy pad looks alert and a neglected
one looks bored, without the persona changing.

The face also **emotes** on events: waking up at boot, a happy bounce when a
host connects, a sad look-around on disconnect, a wink toward the slot on
Easy-Switch, an excited wobble during a fast typing burst, a glance toward the
key you just pressed, and an occasional yawn after a long idle. Emotes are
time-boxed and always yield to pairing mode and the pre-sleep droop, which
report real device state.

### Mode differences

In `"idle"` mode the face is a screensaver: the first press only wakes it and is
never typed. In `"always"` mode the face *is* the main screen — keys type
straight from it with normal tap/hold behaviour, the grid never flashes, and
holding FN opens the SYSTEM menu.

> **Face mode and face screen are different things.** `faceMode` decides whether
> the pad *returns* to the face; `currentScreen` decides what is drawn right now.
> Setting mode `0` over the host link therefore also forces the pad off the face
> screen — without that, a host that turned the face off in order to show the key
> grid would draw its labels behind the eyes, and in `"always"` mode nothing would
> move off the face until a wasted key press woke it. `setPreset`, `setColor` and
> `setStatus` all guard their redraw on the grid being visible, so they are silent
> no-ops while the face is up.

GIF notes: hardware cannot decode MP4/H.264; convert to GIF first
(`ffmpeg -i in.mp4 -vf "fps=12,scale=320:-1" out.gif`). Playback is centered,
looped, ~10–25 fps depending on GIF complexity. While `style` is `"gif"` the
face does not react to state (a canned loop can't); `"eyes"` is the reactive
mode.

On-device controls: **Settings → FACE** cycles OFF/IDLE/ALWAYS, **Settings →
STYLE** toggles EYES/GIF, **Settings → PERSONA** cycles the four personalities.
The config web UI has a Face personality dropdown.

## "Launching apps" — the host-link GATT service

True app-launch happens on the host, not the keyboard. The pad carries a
custom GATT service alongside HID (same bond, same connection) that a
companion app uses for two-way communication. Keys of `type":"host"` send an
*event* over this service instead of typing; the companion decides what to do
(launch, focus, run a script). If no app is subscribed, the key falls back to
its embedded `mod`/`key` chord (if any), so the pad degrades gracefully.

**Service** `6d616372-6f70-6164-0000-000000000001` (ASCII "macropad"):

| Characteristic | UUID suffix | Props | Direction |
|----------------|-------------|-------|-----------|
| Events   | `...0002` | Notify | device → host |
| Commands | `...0003` | Write (encrypted) | host → device |

Wire format both ways: `[opcode:1][len:1][payload:len]`.

**Events (device → host):**

| Op | Name | Payload |
|----|------|---------|
| `0x01` | hello  | `[fwMajor][keys][presets][activePreset][faceMode][persona]` — sent on subscribe |
| `0x02` | key    | `[preset][keyIdx]` — a `host` key was tapped |
| `0x03` | preset | `[preset]` — active preset changed (either side) |
| `0x04` | actions | `[page][totalPages][count]` + `count` × `[id lo][id hi][label 9, null-padded]` — one page of `ACTION_LIB`, in reply to `0x8C` |

**Commands (host → device):**

| Op | Name | Payload | Effect |
|----|------|---------|--------|
| `0x81` | setLabel  | `[preset][key][utf8 ≤8]` | Live label override (RAM only; persists only if the user saves on-device) |
| `0x82` | setStatus | `[utf8 ≤23]` | Status-bar line on the main grid; empty clears |
| `0x83` | setPreset | `[preset]` | Foreground-follow: switch the pad's preset; echoed back as event `0x03` |
| `0x84` | setFace   | `[mode 0-2][persona 0-3]` | Face mode / personality override (RAM only). **Mode 0 also leaves the face screen immediately** — see below |
| `0x8B` | setVolume | `[level 0-100 \| 0xFF unknown][flags: bit0 muted]` | True system volume, relayed to the encoder puck. BLE HID volume is relative, so this is the only way the pad can know the real level |
| `0x8C` | getActions | `[page]` | Ask for one page of the builtin action library; answered with event `0x04` |

### Builtin actions over the host link

`setKey` (`0x86`) with `kaType = 0` (`KA_BUILTIN`) **reuses the consumer field as
the action id** — that is how the companion binds a key to one of the pad's own
actions.

The id list is not duplicated in the companion. It is pulled from the firmware's
`ACTION_LIB` with `getActions`, 8 entries per page, because an 86-entry copy in C#
would drift the moment an action is added to the sketch and the failure mode is a
key silently bound to the wrong action.

Notes: commands are queued in the firmware and applied by the display-owning
loop (bursts of 12 `setLabel`s are fine). `setPreset`/`setLabel` deliberately
do **not** write NVS — the companion pushes state every session, and flash
wear matters. Old alternative for scripts without BLE access: bind a
distinctive `key` chord and use a global-hotkey listener.

### Building on the host link

The **rewrite menu** ([`REWRITE_MENU.md`](REWRITE_MENU.md)) is worth reading as a
worked example: it turns a preset into a transient menu using nothing but the
opcodes above — `setKey` without `commit` to claim the keys in RAM, `setLabel` to
switch the menu between stages, `setFace` 0 to make the grid visible, and `0x02`
key events to drive the state machine. No firmware changes were needed to build
it beyond the `setFace` behaviour documented above.

## Build options for OTA `.bin`

The firmware **must** be compiled with these or OTA images won't match the
partition layout / boot:

```
Board:            ESP32 Dev Module   (esp32:esp32:esp32)
Partition Scheme: CUSTOM — firmware/v5/macropad_v5/partitions.csv is picked up
                  automatically by the esp32 core (dual 1.5MB OTA slots +
                  896KB SPIFFS for GIF animations). The menu setting is
                  ignored when that file is present.
Flash Frequency:  40MHz    (this bench board's Boya flash needs it)
Upload Speed:     115200   (USB flashing only; OTA ignores this)
```

> ⚠️ The switch TO this custom table had to be done once over USB — OTA can
> never change the partition table. After that one flash, OTA works normally
> and images must fit the 1.5MB slot.

arduino-cli one-liner (produces `build/.../macropad_v5.ino.bin`):
```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32:FlashFreq=40,PartitionScheme=min_spiffs" \
  --output-dir build firmware/v5/macropad_v5
```
Upload that `.bin` via `POST /api/update`.
