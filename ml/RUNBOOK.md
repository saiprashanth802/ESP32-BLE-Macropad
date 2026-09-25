# FIX fine-tune — runbook

Everything here is staged and verified. The phases below are the parts that need
GPU, a big download, or a reboot into Linux. Nothing in this file has been run yet.

## State as of 2026-08-16

| | |
|---|---|
| Train set | `data/fix-train.jsonl` — 1074 rows, `2ff886e5…` |
| Eval set (**frozen**) | `data/fix-eval.jsonl` — 100 rows, `cdbfa3a0…` |
| Held-out docs | hardware-handoffs-repo, mixed-fleet-telematics, BOARD_NOTES, DEVICE_STATE, INDEX, fpga-research-zcu104 |
| Pre-flight | PASS — no exact leakage, worst near-dup overlap 14%, 4.4% clean pairs |
| Base models | `Qwen/Qwen2.5-0.5B-Instruct` and `-1.5B-Instruct` (verified to exist, Apache-2.0, not gated) |

**If the eval sha256 ever changes after results have been seen, the scoreboard is
void.** Regenerate and re-baseline, or don't change it.

Regenerating is deterministic (`seed=20260816`) — same inputs give the same files:

```
python make_fix_data.py && python check_data.py
```

---

## Baseline results — MEASURED 2026-08-16

Frozen eval `cdbfa3a0…`, 100 cases, `qwen3:8b`, Ollama 0.32.6.

| | think=on | think=off |
|---|---|---|
| cases scored | **83** (17 timed out at 180 s) | 100 (0 failed) |
| corrections | 111/155 = **71.6%** | 144/182 = **79.1%** |
| spurious changes | 65 = **0.78/case** | 26 = **0.26/case** |
| exact match | 33/83 = **39.8%** | 63/100 = **63.0%** |
| clean text left alone | 1/3 | 5/5 |
| latency median / p95 | **21.1 s / 49.8 s** | **3.3 s / 4.6 s** |

**Thinking off wins on every axis, and by a lot.** It is more accurate, changes
less that it shouldn't, and is 6× faster. Three defects are specific to thinking on:

1. **`/think` leaks into the visible output in 23 of 83 responses (28%).** That
   string would be pasted straight into the user's document on a FIX press.
2. **17 cases exceeded a 180 s timeout** — over three minutes to fix a typo in one
   sentence. Those cases are excluded from the think=on column, so the true
   think=on numbers are *worse* than shown: it was scored on the easier 83.
3. Even discounting every `/think`-contaminated case, spurious changes are 37 vs 26.

Observed failure shape: on `"load-dump lcamp"` (should be `clamp`) it emitted
`"load-dump load-dump"` — a hallucinated correction, not a missed one.

### This contradicts the earlier planted-typo measurement

The `think: true` setting in `styles.json` rests on an earlier result — 4/13 typos
caught with reasoning off, 13/13 on. That is not reproduced here. The two tests are
not directly comparable (that one used planted typos in a longer document; this one
uses 1–4 sentence segments), and Ollama has moved 0.32.5 → 0.32.6 since. **Unresolved
— do not treat either number as settled.**

### Two consequences

- **The bar to beat is now think=off: 79.1% corrections, 0.26 spurious/case, 3.3 s
  median.** Much harder than the ~17 s figure the project was scoped against, and a
  fine-tuned 0.5B is no longer obviously faster than the incumbent.
- **Flipping `"think": false` on FIX looks like a free win available today**, with
  no training at all. Worth trying on real text before assuming the eval generalises.

## Phase A — baseline (Windows, GPU, ~35 min) — DONE

Run when the GPU is free. This is the number the fine-tune has to beat. Ollama must
be running; it is a service, so normally it already is.

```
cd C:\claude_code\soup-lab
python score_fix.py --model qwen3:8b --think on  --out results/baseline-think-on.json
python score_fix.py --model qwen3:8b --think off --out results/baseline-think-off.json
```

Expect ~17 s per case with thinking on (~30 min), a few seconds with it off. Prior
measurement on planted typos: 4/13 with thinking off, 13/13 on.

## RESULT — fine-tune MEASURED 2026-08-16, both models. Verdict: do not ship.

Frozen eval `cdbfa3a0…` (hash re-verified after the NAS copy), 100 cases. Fine-tunes
scored on Linux/Ollama v0.0.0; baselines are the earlier Windows/Ollama 0.32.6 runs.

| | qwen3:8b think=on | qwen3:8b **think=off** | ft 0.5B-4bit | ft 1.5B-4bit |
|---|---|---|---|---|
| cases | 83 (17 timed out) | 100 | 99 (1 failed) | 100 |
| corrections | 71.6% | **79.1%** | 64.1% | 78.6% |
| **spurious/case** | 0.78 | **0.26** | 0.70 | 0.50 |
| exact match | 39.8% | **63.0%** | 36.4% | 52.0% |
| clean left alone | 1/3 | **5/5** | 3/5 | 3/5 |
| latency med / p95 | 21.1 / 49.8 s | 3.3 / 4.6 s | **0.82 / 2.35 s** | 1.85 / 6.11 s |

**Spurious changes is the metric that decides shipping, and both fine-tunes lose on
it** — 0.70 and 0.50 per case against the incumbent's 0.26. The 1.5B ties on
corrections (78.6% vs 79.1%, a one-case difference) and is 1.8x faster, but alters
twice as much that it shouldn't. Both regress on leaving clean text alone (3/5 vs
5/5) — precisely the failure the 4.4% clean-pair share was designed to prevent.

**Therefore: keep `qwen3:8b` and flip `"think": false`.** That was already the
free win; it is now also the best measured option. Phase E ships nothing.

### EPOCHS ARE THE LEVER — 6-epoch run, measured 2026-08-17

Hypothesis (a) below was that 2 epochs is too few. **Confirmed, partially.** Same
config, same data, same frozen eval; `epochs: 2 -> 6` and nothing else
(`probe-1.5b-4bit-e6.yaml`), 22 min:

| | 2 ep | **6 ep** | qwen3:8b think=off |
|---|---|---|---|
| corrections | 78.6% | **82.4%** | 79.1% |
| spurious/case | 0.50 | **0.38** | **0.26** |
| exact match | 52.0% | 60.0% | **63.0%** |
| clean left alone | 3/5 | 3/5 | **5/5** |
| latency median | 1.85 s | **1.73 s** | 3.3 s |
| final train loss | 0.0217 | 0.0028 | — |

Both metrics improved together — corrections now **beat the incumbent**, and the
spurious gap closed by half. That is not what overfitting looks like on the metric,
even though train loss fell to 0.0028, which normally screams memorisation. With no
validation loss computed (see below), train loss cannot distinguish the two here;
**the eval is the only honest signal, and it says 6 > 2.**

### The trend plateaus at ~6 epochs — 12-epoch run, measured 2026-08-17

| | 2 ep | **6 ep** | 12 ep | qwen3:8b think=off |
|---|---|---|---|---|
| corrections | 78.6% | **82.4%** | 81.3% | 79.1% |
| spurious/case | 0.50 | 0.38 | **0.36** | **0.26** |
| exact match | 52.0% | 60.0% | **64.0%** | 63.0% |
| clean left alone | 3/5 | 3/5 | 3/5 | **5/5** |
| latency median | 1.85 s | 1.73 s | **1.64 s** | 3.3 s |
| train time | 7 min | 22 min | 44 min | — |
| final train loss | 0.0217 | 0.0028 | 0.0009 | — |

**Doubling 6 -> 12 bought nothing worth 22 extra minutes**: corrections fell 1.1 pts,
spurious fell 0.02, exact match rose 4 pts. The gain is all between 2 and 6.

This also **refutes the comment carried in `soup-0.5b.yaml`**: `epochs: 2  # 3 overfits
to the corruption script's habits`. That was an assumption, never measured. Six epochs
is better than two on both metrics, and twelve shows no overfitting penalty on the
eval despite memorising the training set. The 2-epoch default cost real performance.

**Epochs are exhausted as a lever.** Do not run 24 — the remaining gap on the
deciding metric (0.36 vs 0.26) is not an optimisation-budget problem. Train loss at
0.0009 means the model has memorised the training set; that it still will not stop
making unrequested edits says the behaviour is coming from the **data distribution**,
not from insufficient fitting.

### The 0.5B is capacity-limited, not epoch-limited

Same 2 -> 6 epoch change applied to the 0.5B (`probe-0.5b-4bit-e6.yaml`, 8 min):

| 0.5B | 2 ep | 6 ep | (1.5B @6ep) | (incumbent) |
|---|---|---|---|---|
| corrections | 64.1% | 69.2% | 82.4% | 79.1% |
| spurious/case | 0.70 | 0.62 | 0.38 | **0.26** |
| exact match | 36.4% | 44.0% | 60.0% | 63.0% |
| clean left alone | 3/5 | 2/5 | 3/5 | **5/5** |
| latency median | 0.82 s | **0.81 s** | 1.73 s | 3.3 s |

Epochs help the 0.5B in the same direction (+5.1 pts corrections, -0.08 spurious) but
it never gets close: 10 pts behind the incumbent on corrections and 2.4x its spurious
rate. **The 0.5B is out.** Its only advantage is 0.81 s median latency, and FIX does
not need sub-second when the alternative is 1.7 s and far more accurate.

**Full sweep complete: 4 configs x 2 sizes, all against the frozen eval. The best
model in this project is the 1.5B at 6 epochs.**

### Restraint probe at the real register — 6-epoch and 12-epoch, 2026-08-17

The same 6-case hand probe (temp 0, via Ollama) re-run against the two best models.
**Both scored perfectly:**

| | clean left alone | identifiers preserved | typos fixed to reference |
|---|---|---|---|
| ft-1.5b-4bit-e6 | **3/3** | **1/1** | 2/2 |
| ft-1.5b-4bit-e12 | **3/3** | **1/1** | 2/2 |

Clean sentences untouched, `A4:C1:38:9F:2B:11` and `/srv/media/music` preserved,
`num_ctx` untouched, and both typo cases corrected to exactly the reference — including
`I recieved teh package yesterday.`, which the 2-epoch 0.5B had paraphrased into
`Yesterday I received the package.`

**This matters for how the 0.38 spurious rate should be read.** That figure comes
entirely from dense technical documentation prose. At the register FIX actually gets
— short messages, notes, emails — the 6-epoch model shows no restraint failure at all
in this sample. **n=6 proves nothing on its own**, which is exactly why the ~20 real
snippets are the highest-value outstanding item: they would decide whether the ship
criterion is failed in practice or only on an unrepresentative eval.

### Where this leaves the decision

At 6 epochs the fine-tune **beats the incumbent on corrections (82.4% vs 79.1%),
ties it on exact match at 12 epochs (64.0% vs 63.0%), and is 2x faster** (1.64-1.73 s
vs 3.3 s median). It loses on exactly one axis: restraint — 0.36-0.38 spurious
changes per case against 0.26, and 3/5 vs 5/5 on leaving clean text alone.

**The recommendation is unchanged: keep `qwen3:8b` and flip `"think": false`.**
Restraint is the ship criterion, the fine-tune loses on it, and the incumbent costs
one line of JSON and no training. But the margin is now narrow enough that the
project is no longer a dead end — it is one dataset fix away from a real decision.

**The one untested hypothesis, and it is the user's call because it changes the data:**
95.6% of training pairs contain an error, so the model learns that an edit is nearly
always required. Raising the clean-pair share (4.4% -> ~20%) in the **train split only**
directly targets the only metric still losing. Rebuild train, keep `data/fix-eval.jsonl`
and its `cdbfa3a0…` hash frozen, re-run at 6 epochs, compare against this table.
Note that `clean left alone` is n=5 — far too small to conclude from either way, which
is another reason the real-register snippets matter.

### What the failures actually are — probed 2026-08-16

Word-level diff of prediction vs reference, bucketed:

| | 1.5B-4bit | 0.5B-4bit |
|---|---|---|
| word substitution | **54** | **74** |
| case-only | 3 | 6 |
| punctuation | 1 | 5 |
| deletion / insertion | 1 / 1 | 3 / 3 |

Nothing is truncated (checked explicitly). The failure is not missed typos — it is
**paraphrasing**: `will`→`may`, `is`→`are`, `stronger`→`standard`, `sits`→`weighs`,
`set a DTC`→`steal DTCs`, `reads`→`eradates`. The 0.5B additionally breaks the
identifier protection the generator was built to teach (`load-dump clamp` →
`load dump camp`, de-hyphenating and corrupting a technical term).

**The masking hypothesis for this is dead** (masking works — see below). Remaining
candidates: (a) 2 epochs on 1074 rows is too little to override the base model's
instruct-tuned urge to rewrite, (b) the corruption distribution teaches "something is
always wrong here" — 95.6% of training pairs do contain an error, so the prior the
model learns is that an edit is nearly always required.

**Sampling noise is ruled out**: `score_fix.py` already calls Ollama with
`temperature: 0`, so every result above is greedy decoding. (b) is the more
interesting hypothesis and the cheaper test — raising the clean-pair share from 4.4%
to ~20% is a data change, not a training change, and `make_fix_data.py` regenerates
deterministically. **But that changes the eval hash and voids the scoreboard unless
the eval split is held fixed while only the train split is rebalanced.**

### Spurious changes vs input length, and a direct probe

Mean spurious/case by input-length quartile:

| | shortest 25% (~55 ch) | mid 50% (~133 ch) | longest 25% (~275 ch) |
|---|---|---|---|
| qwen3:8b think=off | **0.16** | **0.24** | **0.40** |
| ft 1.5b-4bit | 0.32 | 0.64 | 0.40 |
| ft 0.5b-4bit | 0.62 | 0.67 | 0.81 |

**The fine-tunes lose in every band, including the shortest.** The 0.5B degrades
monotonically with length; the 1.5B is worst in the middle band. So this is not a
"documentation prose is the wrong register" artefact that real-register text would
rescue — it is worse everywhere.

Against that, a 6-case hand probe of short real-register text (`ollama`, temp 0)
came out well: both models left clean sentences untouched 3/3, preserved a MAC
address and a `/srv/media/...` path, and fixed `recieved`/`teh` and missing
capitalisation correctly. The 1.5B was clean on all six; the 0.5B's one flaw was a
paraphrase (`I recieved teh package yesterday.` -> `Yesterday I received the
package.`). Six cases prove nothing statistically, but they suggest the models are
usable on short everyday text and fall apart on longer dense technical prose —
which is exactly where the **~20 real-register eval snippets** would settle the
question. That outstanding item is now the highest-value next step, not an optional
extra.

### Per-defect-class correction rate

Heuristic classification (diff `input` vs `reference`, then check whether the corrected
token appears in the prediction). Approximate — it does not reproduce `score_fix.py`'s
counting exactly, and the "deleted word" class cannot be verified this way at all
(it scores 0 for every model including the incumbent, an artefact — ignore it).

| defect class | qwen3:8b off | ft-1.5b-4bit | ft-0.5b-4bit |
|---|---|---|---|
| case | 19/19 (100%) | 19/19 (100%) | 19/19 (100%) |
| dropped/doubled letter | 4/4 (100%) | 4/4 (100%) | 4/4 (100%) |
| punctuation | 32/33 (97%) | **33/33 (100%)** | 28/32 (88%) |
| other (homophone/word choice) | 48/60 (80%) | 48/60 (80%) | 43/60 (72%) |
| transposition | **39/52 (75%)** | 37/52 (71%) | 32/52 (62%) |
| apostrophe | **1/4 (25%)** | 0/4 (0%) | 0/4 (0%) |

**The 1.5B has learned the mechanical classes.** It beats the incumbent on
punctuation, ties on case, dropped letters and word choice, and loses only on
transposition (71% vs 75%) and apostrophes (0/4, though n=4 decides nothing).

This sharpens the verdict: the fine-tune is **competitive at fixing and loses purely
on restraint**. Everything hinges on suppressing edits it should not make — which
points at the training distribution (95.6% of pairs contain an error) rather than at
capacity or at the fix behaviour itself.

Caveats on these numbers, in order of how much they could matter:
- Both fine-tunes are **4bit**; the bf16 configs never ran (VRAM, see below).
- Export is **f16 GGUF, not q8_0** — `llama-quantize` is not built in Soup's bundled
  llama.cpp. f16 is the lower-error choice, so this does not disadvantage the models.
- Latency columns cross machines/runtimes (Linux Ollama v0.0.0 vs Windows 0.32.6);
  correction and spurious counts are text-diff metrics and are unaffected.
- The eval remains 100% documentation prose. The ~20 real-register snippets are
  still outstanding and would change what any of this predicts about real FIX use.

## Soup behaviour — VERIFIED 2026-08-16 on Linux, corrects the phases below

First real exercise of the `soup` toolchain. Four things do not work as this
runbook assumed. All measured, not inferred.

1. **`soup train` prompts for confirmation — pass `--yes`.** Without a TTY it prints
   `Start training? [Y/n]: Aborted.` and exits 0, so a backgrounded run looks like it
   started and silently did nothing.
2. **`soup doctor` cannot be a go/no-go before the extras.** On a bare `soup-cli`
   install it reports the seven training packages as MISSING issues — the very ones
   the next step installs. Only the System/GPU/Resources panels are meaningful until
   `soup-cli[train]` is in; after that it reports `All checks passed`.
3. **Validation loss is never computed.** `val_split: 0.1` builds the split (108 rows,
   visible in the "Truncating eval dataset" step) but **zero `eval_loss` lines are
   emitted**. Phase C's "watch validation loss against training loss" is not possible
   as written; training loss and `mean_token_accuracy` are the only signals.
4. **Completion-only loss masking IS in effect** — this resolves the HANDOFF's open
   question, and the answer is the reassuring one. The alarming log line
   (`return_assistant_tokens_mask==True but chat template does not contain
   '{% generation %}' keyword`) is Soup's *preferred* masking path failing; it then
   falls back to a per-turn token-delta method that masks correctly.

   Measured directly on train row 1 via `soup_cli.data.loss_mask.build_assistant_only_labels`
   (note the signature is `(messages, tokenizer)`, in that order):

   ```
   tokens=192   masked(-100)=163   in-loss=29  (15.1%)
   in-loss text: '<|im_start|>assistant\nOnly for an account that should be…<|im_end|>\n'
   ```

   Only the assistant response is in the loss. The fallback is looser than the
   preferred path by exactly the role-prefix tokens (`<|im_start|>assistant\n`), which
   its own docstring states. `train_on_responses_only` defaults to `True`.
   **So the spurious-change rate below is NOT explained by a masking defect.**

Resolved stack (all floors, nothing pinned — almost certainly not what Soup 0.73.2
was tested against): torch 2.13.0+cu130, transformers 4.57.6, trl 0.28.0, peft 0.20.0,
datasets 5.0.1, bitsandbytes 0.50.1, accelerate 1.14.0, Python 3.12.13 (uv-managed).

### The 8 GB VRAM wall — the 1.5B does not fit in bf16

On the RTX 4060 (7.6 GB usable, GPU verified idle at 49 MiB each time):

| attempt | result |
|---|---|
| `max_length: 512`, `quantization: none` (config as written) | **OOM at batch_size=1** |
| `max_length: 256` | **OOM at batch_size=1** |
| `gradient_checkpointing: true` at 512 | **OOM at batch_size=1** |
| `quantization: 4bit` at 512 | trains, 7 min |

A 1.5B + LoRA in bf16 should fit 8 GB with room to spare. That it does not — plus
allocator OOM warnings appearing even on the **0.5B** — indicates the base is loaded
in **fp32** despite `quantization: none` (1.5B fp32 ≈ 6.2 GB of weights alone, which
matches the failure exactly). Disk is irrelevant here; it is a VRAM limit.

**Consequence for the comparison:** 4bit is the only lever that works, so both sizes
must run at 4bit or the result confounds model size with precision. `probe-0.5b-4bit.yaml`
and `probe-1.5b-4bit.yaml` are the matched pair (identical but for `base`/`output`);
the original `soup-0.5b.yaml`/`soup-1.5b.yaml` pair is unrunnable at 1.5B.

## Phase B — environment (Linux, ~10 min + several GB download)

Requires rebooting into the Linux partition. Windows Python is 3.14, too new for
the training stack; the only 3.12 is a Microsoft Store build, a poor venv base.

```
curl -LsSf https://astral.sh/uv/install.sh | sh
uv venv --python 3.12 ~/soup-lab/.venv
source ~/soup-lab/.venv/bin/activate
uv pip install soup-cli
soup doctor
```

`soup doctor` is the go/no-go. Only after it is clean:

```
uv pip install "soup-cli[train]"
```

Copy `data/`, `soup-0.5b.yaml`, `soup-1.5b.yaml` across. **Nothing else from
`hardware-handoffs` leaves the machine** — it is private, and the generated pairs
are the only thing training needs.

## Phase C — train the sweep (Linux, GPU)

```
soup train --config soup-0.5b.yaml
soup train --config soup-1.5b.yaml
```

The two configs differ **only** in `base` and `output`. Keep it that way or the
comparison stops being one. Watch validation loss against training loss — if val
rises while train falls, cut epochs or add data.

## Phase D — export and score (both, GPU)

```
soup export --model ./output/macropad-fix-0.5b --format gguf --quant q8_0 --deploy ollama
soup export --model ./output/macropad-fix-1.5b --format gguf --quant q8_0 --deploy ollama
```

`soup export` auto-merges the LoRA adapter into the base before converting, so the
result is a standalone GGUF — no adapter juggling at inference. `q8_0` rather than
`q4_k_m` because these models are small enough that the disk saving is irrelevant
and quantisation error is not.

Then score both against the **same frozen set**:

```
python score_fix.py --model macropad-fix-0.5b --out results/ft-0.5b.json
python score_fix.py --model macropad-fix-1.5b --out results/ft-1.5b.json
```

Compare three numbers across all four runs, and do not average them together:

- **corrections** — did it catch the errors that were there?
- **spurious changes** — did it alter anything it shouldn't have? *This is the one
  that decides whether it ships.*
- **latency** — median and p95, against the ~17 s baseline.

## Phase E — deploy (Windows, no rebuild)

Edit **only** the FIX entry in `%USERPROFILE%\MacroPadDeck\styles.json`:

```json
{ "key": 5, "label": "FIX", "model": "macropad-fix-1.5b", "think": false, "temperature": 0 }
```

`think: false` — the behaviour is in the weights now. `styles.json` hot-reloads, so
no rebuild is needed (and rebuilds are the risky path — exit MacroPadDeck from the
tray first if one is ever required).

Rollback: set `"model": ""` back. Nothing else in the app changes.

---

## Still outstanding

- **~20 real-register eval snippets** from the user (emails, messages, notes — the
  text FIX actually gets, typos included). The current eval is 100% documentation
  prose, which may not predict real use. Becomes `data/fix-eval-real.jsonl`, scored
  alongside the synthetic set. **This is now the highest-value open item** — the
  per-length and per-class breakdowns above both point at it.

  **Scaffolding is built and smoke-tested (2026-08-16): `build_real_eval.py`.**
  Paste snippets into a plain text file as blank-line-separated two-line blocks
  (line 1 as typed, line 2 as it should be), then:

  ```
  python build_real_eval.py real_snippets.txt
  python score_fix.py --model soup-macropad-fix-1.5b-4bit \
                      --data data/fix-eval-real.jsonl \
                      --out results/ft-1.5b-real.json
  python score_fix.py --model qwen3:8b --think off \
                      --data data/fix-eval-real.jsonl \
                      --out results/baseline-real.json
  ```

  It copies the instruction string verbatim from the frozen eval (never retyped, so
  the two sets stay comparable), reports a sha256 to freeze, and writes to a separate
  file — `data/fix-eval.jsonl` and its `cdbfa3a0…` hash are untouched.

  Two things that will silently ruin the file: **paraphrasing in line 2** (the
  reference must differ from line 1 only by the corrections, or every model is
  scored against a rewrite it was never asked to produce), and using invented typos
  instead of real ones. Include a few already-correct snippets — restraint is the
  metric that decides this whole question, and only clean inputs measure it.
- **Loss masking is unverified.** Soup's docs do not say whether SFT loss is
  computed on the completion only or the whole sequence. It does not change the
  config, but it does change how much the instruction text influences training —
  worth checking against a short run before reading too much into the numbers.
