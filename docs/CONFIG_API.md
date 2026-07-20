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
| GET  | `/api/config`   | Full config (settings + all presets) |
| POST | `/api/config`   | Apply + persist config (JSON body) |
| POST | `/api/update`   | OTA firmware upload (multipart `.bin`) |
| POST | `/api/exit`     | Save nothing, reboot to keyboard mode |

### `GET /api/info`
```json
{ "device":"ESP32 MacroPad", "fw":"v5-multihost", "api":2,
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

## "Launching apps"

True app-launch happens on the host, not the keyboard. Two supported patterns:

1. **OS launcher macro** — e.g. a `macro` of `Gui+R` → (companion isn't needed
   if the OS run box accepts typed text; note `text` steps aren't inside macros
   yet, so use a `text` key or a launcher shortcut you've bound in the OS).
2. **Companion hotkey** — bind a distinctive `key` chord (e.g. Ctrl+Alt+F13-ish)
   and have your companion app/script listen for it and launch the app. This is
   the most robust cross-platform route and what the app is expected to use.

## Build options for OTA `.bin`

The firmware **must** be compiled with these or OTA images won't match the
partition layout / boot:

```
Board:            ESP32 Dev Module   (esp32:esp32:esp32)
Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)   [min_spiffs]
Flash Frequency:  40MHz    (this bench board's Boya flash needs it)
Upload Speed:     115200   (USB flashing only; OTA ignores this)
```

arduino-cli one-liner (produces `build/.../macropad_v5.ino.bin`):
```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32:FlashFreq=40,PartitionScheme=min_spiffs" \
  --output-dir build firmware/v5/macropad_v5
```
Upload that `.bin` via `POST /api/update`.
