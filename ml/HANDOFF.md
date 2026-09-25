# soup-lab — fine-tuning a local FIX model for the macropad

Handoff written 2026-08-16. **Updated 2026-08-16 (later, on Linux): both models are
now trained, exported, deployed and scored. The result is negative — do not ship.**
See the RESULT section at the top of `RUNBOOK.md` for the numbers; the summary is:

- ft 1.5B-4bit ties the incumbent on corrections (78.6% vs 79.1%) and is 1.8x faster,
  but **doubles the spurious-change rate (0.50 vs 0.26/case)** — the metric this
  document names as the one that decides shipping. ft 0.5B-4bit is worse still (0.70).
- **Then epochs turned out to be a real lever** (2026-08-17). At **6 epochs** the 1.5B
  reaches **82.4% corrections — beating the incumbent's 79.1%** — with spurious down to
  0.38; at 12 epochs, 81.3% / 0.36 / 64.0% exact (vs the incumbent's 63.0%), ~2x faster
  throughout. The trend plateaus at ~6; **do not escalate epochs further.** The
  `epochs: 2  # 3 overfits` comment in the shipped configs is refuted — it was an
  assumption and it cost performance.
- So the fine-tune now **wins on corrections, ties on exact match, wins on speed, and
  loses only on restraint** (0.36 vs 0.26 spurious, 3/5 vs 5/5 clean left alone).
  Narrow enough that this is no longer a dead end — but restraint is the ship
  criterion, so the recommendation still holds.
- Both regress on leaving clean text alone (3/5 vs the incumbent's 5/5).
- **The recommendation is unchanged and is now the measured best option: keep
  `qwen3:8b` and flip `"think": false`.** No training required.
- The open question below about loss masking is **resolved: masking works** (verified
  directly — only the assistant response is in the loss). It is not the explanation
  for the spurious-change rate.

Everything else below is as originally written — either measured on hardware or
explicitly marked as unverified.

Source of truth for commands: `RUNBOOK.md` beside this file.

---

## Problem

The pad's rewrite menu sends every style to `qwen3:8b` via Ollama except EMAIL and
ASK, which go to `gpt-oss:120b-cloud`. FIX was believed to be the slowest style at
~17 s, because it runs `"think": true` in `styles.json`.

The goal was a small task-specific model that does FIX offline in well under a
second, and — more importantly — a proving run for a pipeline that later covers a
router, the tone styles, and distilling the cloud model.

FIX was chosen first because it is the **only** style whose training data can be
generated programmatically with perfect ground truth: corrupt clean text, and
`(corrupted → clean)` is the pair. No hand-written judgement calls.

## Constraints

- **Windows box cannot train.** Default Python is 3.14 (too new for the stack); the
  only other interpreter is a Microsoft Store 3.12, a poor venv base. Hence Linux.
- Laptop RTX 4060, **8 GB VRAM**, 74 GB free on C:.
- Deployment must not require an app rebuild — rebuilds are the risky path.
  `styles.json` has a per-style `model` field and hot-reloads, so the ship step is
  one line and the rollback is `"model": ""`.
- The corpus is the user's own private prose (`hardware-handoffs`, memory files,
  `project-ideas`). It stays local. Generated pairs are the only thing training needs.

## Investigation

### What was built (all verified working)

| File | What it does |
|---|---|
| `make_fix_data.py` | Corruption generator. Stdlib only, deterministic (`seed=20260816`). Runs fine on Python 3.14. |
| `check_data.py` | Pre-flight: schema, exact leakage, shingle near-dup leakage, clean-pair share, eval hash. |
| `score_fix.py` | Eval harness against Ollama. Reports corrections and spurious changes **separately**. |
| `soup-0.5b.yaml` / `soup-1.5b.yaml` | Training configs. Differ **only** in `base` and `output`. |

Output: **1074 train rows, 100 eval rows.** Pre-flight PASS.
Hashes — eval `cdbfa3a074cff0a3…`, train `2ff886e51138743b…`.

### The leakage trap in this corpus, and the fix

Splitting train/eval **by document is not sufficient here.** The memory files are
mirrors of the handoff docs (that is what `sync-memory.sh` does), the two copies
have drifted by a few words, and exact-match dedup lets the near-twins through.

`check_data.py` caught it on the first run: one eval row shared >50% of its 5-word
shingles with training. `drop_leaks()` now filters eval segments against training
text at a 0.3 threshold; worst remaining overlap is 14%.

**This will recur** if the corpus is extended. Always run `check_data.py`.

### Corruption classes

Modelled on typos humans actually make — transposition, dropped/doubled letter,
lowercase sentence start and bare `i`, missing terminal punctuation, missing
apostrophe, homophone, doubled word. Random character noise was deliberately
avoided: it teaches the model to fix a distribution that does not exist.

Identifiers are protected from corruption — inline code, MAC addresses, hex, paths,
snake_case, camelCase, acronyms, anything containing a digit. Teaching the model
**not** to touch `num_ctx` or a device MAC is as important as teaching it to fix
`teh`.

**~5% of pairs are already-correct text.** Without these the model learns there is
always something to fix and starts rephrasing clean input — the same failure shape
as the original FIX prompt echoing its input.

### Baseline — MEASURED on hardware 2026-08-16

`qwen3:8b`, Ollama 0.32.6, frozen 100-case eval.

| | think=on | think=off |
|---|---|---|
| cases scored | **83** (17 timed out at 180 s) | 100 (0 failed) |
| corrections | 71.6% | **79.1%** |
| spurious changes | 0.78/case | **0.26/case** |
| exact match | 39.8% | **63.0%** |
| clean text left alone | 1/3 | 5/5 |
| median / p95 latency | 21.1 s / 49.8 s | **3.3 s / 4.6 s** |

**Thinking off wins on every axis.** Three defects are specific to thinking on:

1. **`/think` leaks into the visible output in 23 of 83 responses (28%)** — that
   string would be pasted into the user's document on a real FIX press.
2. **17 cases exceeded a 180 s timeout.** They are excluded from the think=on
   column, so those figures are computed on the easier 83 and **flatter it**.
3. Hallucinated corrections, e.g. `"load-dump lcamp"` (should be `clamp`) became
   `"load-dump load-dump"`.

### Unresolved contradiction

The `"think": true` setting rests on an earlier informal result — 4/13 planted typos
caught with reasoning off, 13/13 on. **That did not reproduce.** The tests are not
directly comparable (planted typos in a longer document vs 1–4 sentence segments)
and Ollama moved 0.32.5 → 0.32.6 in between. Neither number is settled; do not
quietly adopt either.

## Resolution — and what it costs the plan

The premise weakened. "FIX takes 17 s" was one of four reasons to do this task
first, and **the real bar is think=off: 79.1% corrections, 0.26 spurious/case,
3.3 s median.** A fine-tuned 0.5B is no longer obviously faster than the incumbent.

What still justifies the work:

- **0.26 spurious changes per case** means roughly a quarter of FIX presses alter
  something they should not. That is the number worth training against.
- Fully offline, and consistent — no thinking-length variance, no timeouts.
- It is still the cheapest possible proving run for the wider pipeline.

**Free win not yet tried:** flipping `"think": false` on the FIX style. One line,
hot-reloads, no training. Should be tested on real text before trusting the eval
to generalise.

## Consequences

- If the corpus is extended or the split changed, **the eval hash changes and the
  baseline above is void.** Re-measure before comparing anything to it.
- The eval is 100% documentation prose. Real FIX presses are emails, messages and
  notes — a different register. **~20 real snippets are still outstanding** and the
  scoreboard is weaker without them.
- The two training configs must stay identical apart from `base`/`output`, or the
  0.5B-vs-1.5B comparison stops being a comparison.

---

## Carrying over to Linux

**This markdown alone is not sufficient.** Also copy:

```
make_fix_data.py  check_data.py  score_fix.py
soup-0.5b.yaml    soup-1.5b.yaml
data/fix-train.jsonl  data/fix-eval.jsonl
RUNBOOK.md
```

The data can be regenerated deterministically instead — but only on a machine that
has the source corpus, so copying the two JSONL files is simpler and guarantees the
hashes still match the baseline above.

Then, on Linux:

```
curl -LsSf https://astral.sh/uv/install.sh | sh
uv venv --python 3.12 ~/soup-lab/.venv
source ~/soup-lab/.venv/bin/activate
uv pip install soup-cli
soup doctor                      # go/no-go before the multi-GB extras
uv pip install "soup-cli[train]"
python check_data.py             # confirm hashes survived the copy
soup train --config soup-0.5b.yaml
soup train --config soup-1.5b.yaml
```

Export merges the LoRA adapter into the base automatically, so the GGUF is
standalone:

```
soup export --model ./output/macropad-fix-0.5b --format gguf --quant q8_0 --deploy ollama
python score_fix.py --model macropad-fix-0.5b --out results/ft-0.5b.json
```

## Verified vs assumed

**Verified on hardware 2026-08-16:** dataset generation, pre-flight checks, the
eval harness, and both baseline runs above. Ollama is **not** a registered Windows
service — it is the tray app, and it was not running; started with
`ollama serve` detached.

**From documentation only, never exercised:** every `soup` command. Soup is a young
project (v0.73.2, effectively one maintainer) and its README admits a past silent
gradient bug on NF4 models >165 MB/layer. Validate loss curves rather than trusting
them. `soup doctor` is the first real test of whether any of this works.

**Unverified and worth checking during the first run:** whether Soup's SFT loss is
masked to the completion only or spans the whole sequence. Undocumented. It does not
change the config, but it changes how much the instruction text influences training.

> **RESOLVED 2026-08-16 — masked to the completion only.** `train_on_responses_only`
> defaults to `True`. Measured on train row 1: 163 of 192 tokens are `-100`, and the
> 29 in-loss tokens decode to exactly the assistant response (plus the
> `<|im_start|>assistant` role prefix). The scary log line
> `return_assistant_tokens_mask==True but chat template does not contain
> '{% generation %}'` is Soup's *preferred* path failing and falling back to a
> token-delta method that masks correctly but looser by those few prefix tokens.
> Do not read that warning as "masking is broken" — it was, briefly, read that way.
