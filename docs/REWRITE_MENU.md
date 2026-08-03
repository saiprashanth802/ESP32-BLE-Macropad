# Rewrite Menu — local LLM text tools on the pad

Select text anywhere, press a key on the pad, pick a style, and the pad rewrites
it in place. Everything runs against a local [Ollama](https://ollama.com)
instance, so no text leaves the machine unless you deliberately point a style at
a hosted model.

The pad is the **input device** and the monitor is the **display**: a rewritten
paragraph will never fit a 240 px screen, but your hand is already on the pad, so
picking a style and accepting the result belong there.

Implemented in the Windows companion app — `MacroPadDeck.Core/WriteFlow.cs`,
`StyleStore.cs`, `LlmClient.cs`, plus `WindowsTextCapture.cs` and
`RewritePreviewWindow.cs`. No firmware support is required beyond `type: "host"`
keys, which already exist.

---

## Requirements

- **Ollama** running locally (default `http://localhost:11434`)
- At least one model pulled, e.g. `ollama pull qwen3:8b`

An 8B model at Q4 needs roughly 5 GB of VRAM. Smaller models work fine for the
mechanical styles; see [Choosing models](#choosing-models).

---

## Setup

**1. Pick a preset for the menu.** `styles.json` → `writePreset` (default `7`).

This has to be a **dedicated** preset. Its 12 keys are claimed at runtime as the
style picker. Keys left as `"none"` in `profiles.json` are firmware defaults the
app cannot read back, so the menu can only safely own a preset the app fully
defines. Nothing is written to flash — the claim uses `setKey` (`0x86`) with no
`commit` (`0x87`), so `profiles.json` is untouched and setting `writePreset: -1`
gives the preset back on the next connect.

**2. Add a trigger key** to any *other* preset, in `profiles.json`:

```json
{ "type": "write", "label": "WRITE" }
```

> The editor's binding-type dropdown does not offer `write` yet, so this is a
> hand edit. `profiles.json` hot-reloads, so no restart is needed.

You can also skip the trigger key entirely and just switch to the write preset by
hand — pressing a style key there captures the selection at that moment.

---

## Using it

**Stage 1 — pick a style.** The pad switches to the write preset and shows:

```
FORMAL   CASUAL   EMAIL    SHORTER
CLEARER  FIX      ASK      SUMMARY
  —        —        —      BACK
```

**Stage 2 — review.** The rewrite appears on screen, original on the left,
result on the right and editable. The pad keys become:

```
ACCEPT   REGEN    CANCEL     —
  —        —        —        —
  —        —        —       BACK
```

`ACCEPT` pastes over the original selection. `BACK` returns to the style list.
On the keyboard: **Ctrl+Enter** accepts, **Esc** cancels. The menu closes itself
after `menuTimeoutSeconds` of no input.

The pad's status line reports each step (`PICK STYLE`, `LOADING MODEL`,
`REVIEW ON SCREEN`, `PASTED`).

### Default styles

| Key | Label | What it does |
|-----|-------|--------------|
| 0 | `FORMAL` | Same content, professional register |
| 1 | `CASUAL` | Same content, relaxed register |
| 2 | `EMAIL` | Expands notes into a full email; returns a subject separately |
| 3 | `SHORTER` | Cuts to roughly half length |
| 4 | `CLEARER` | Untangles sentences, main point first |
| 5 | `FIX` | Spelling, grammar and punctuation only |
| 6 | `ASK` | Treats the selection as a prompt and answers it |
| 7 | `SUMMARY` | Condenses, bullets for longer input |

`EMAIL` emits `{ "subject", "body" }` via schema-constrained decoding. The body
is pasted; the subject goes to the clipboard, since it belongs in a different
field than the one you selected in.

---

## `styles.json`

Lives beside `profiles.json` in `%USERPROFILE%\MacroPadDeck\`. **Hot-reloads** —
edit and save, and the pad picks it up in under a second. No rebuild.

### Top level

| Field | Default | Purpose |
|---|---|---|
| `endpoint` | `http://localhost:11434` | Ollama base URL |
| `model` | `qwen3:8b` | Default model for every style |
| `writePreset` | `7` | Pad preset the menu owns; `-1` disables the feature |
| `requestTimeoutSeconds` | `120` | Per-generation ceiling |
| `menuTimeoutSeconds` | `45` | Idle timeout before the menu closes itself |
| `keepAlive` | `"-1"` | How long Ollama holds the model. `-1` = until unloaded |
| `autoLoad` | `true` | Load the model on demand if it isn't resident |
| `disableThinking` | `true` | Global default for skipping a model's reasoning pass |
| `restoreFaceMode` | `2` | Face mode to restore if the pad is found with the face off |

### Per style

| Field | Purpose |
|---|---|
| `key` | Pad key index `0..11` (key 11 is reserved for `BACK`) |
| `label` | Pad label, ASCII, ≤ 8 chars |
| `system` | The system prompt — the thing that actually decides output quality |
| `model` | Optional per-style override of the top-level `model` |
| `temperature` | `0` is stilted for prose; 0.2–0.4 reads more natural |
| `think` | Per-style override of `disableThinking`; `null` follows the global |
| `structured` | Ask for `{subject, body}` JSON instead of prose (EMAIL only) |
| `examples` | Few-shot `{in, out}` pairs — worth more than longer instructions |

---

## Choosing models

Ollama serves hosted models through the *same* local endpoint, marked by a
`-cloud` tag, so mixing them is just a different string:

```json
{ "key": 2, "label": "EMAIL", "model": "gpt-oss:120b-cloud" }
```

A sensible split is mechanical styles local, generative styles hosted:

| Styles | Model | Why |
|---|---|---|
| `FIX`, `SHORTER`, `CLEARER`, `FORMAL`, `CASUAL` | local | private, offline, fast enough |
| `EMAIL`, `ASK` | hosted | generation is where a small model is visibly weaker |

⚠ **Hosted styles send your text to a third party and need internet.** They fail
with `LLM OFFLINE` when it's unavailable while local styles keep working. Keep
the split visible so you always know which key is which.

---

## Limits and gotchas

**Ollama's default context is 4096 tokens.** Longer input is silently truncated
with no error, which yields a confident summary of half a document. Roughly 2800
tokens of input is the safe ceiling. Hosted models have far larger contexts and
cost no local VRAM, which is the cheapest fix.

**`keep_alive` must be a JSON number, not a string.** Ollama parses strings as Go
durations, so `"-1"` fails every request with `time: missing unit in duration`.
Unit strings like `"30m"` are fine.

**Reasoning models need `think` set per style, not globally.** Measured on
`qwen3:8b`: proofreading caught 4 of 13 planted typos with thinking off and 13 of
13 with it on, while tone rewrites were 2.4× faster with it off and no worse.
Models with no reasoning pass reject the flag; the client retries without it.

**Lead prompts with the action, not the prohibition.** An early `FIX` prompt
opened with *"change nothing else… leave it byte-for-byte unchanged"* and the
model echoed its input untouched at every temperature. Same shape in `EMAIL`: a
blanket *"use only facts present in the input"* made it wrap notes in a greeting
instead of writing prose. Expanding the *language* and inventing *facts* have to
be stated as separate rules.

**Capture is clipboard-based.** UI Automation's `TextPattern` is not exposed by
Electron apps or browser `contenteditable` (i.e. Gmail), whereas Ctrl+C works
anywhere text can be selected. The original clipboard text is restored after a
paste; non-text clipboard contents cannot be preserved.

**Paste is abandoned if focus can't be restored.** The preview window takes focus,
so the target window is snapshotted and explicitly refocused before pasting. If
that fails the paste is skipped — landing a rewritten paragraph in the wrong
application is worse than doing nothing.

---

## How it interacts with the pad

- **The face is suppressed while the menu is open** (`setFace` mode 0) so the key
  grid is readable, and restored on close. If the app is killed mid-menu the pad
  keeps the face off; the next connect detects this and puts it back.
- **Profile auto-follow is suspended while the menu is open**, or the preview
  window taking focus would switch presets out from under it.
- **Stage 2 relabels with `setLabel` only**, never `setKey` — labels are
  restorable from `styles.json`, key types are not. Labels are diffed against a
  cache because a BLE label write costs 30–60 ms.

See [`CONFIG_API.md`](CONFIG_API.md) for the underlying host-link opcodes.
