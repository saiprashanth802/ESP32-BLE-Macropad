# Fine-tuning a FIX model for the pad — and why it did not ship

The rewrite menu's **FIX** style has the narrowest job on the pad: correct
spelling, grammar and punctuation in the selected text, and change *nothing
else*. It is also the style most likely to be pressed on a MAC address, a file
path or a `snake_case` identifier, where a helpful rewrite is a corruption.

That narrowness is exactly the shape a small fine-tune is supposed to win: one
task, a fixed system prompt, no need for world knowledge. So it was tried
properly — a matched sweep across two model sizes and four epoch counts, scored
against a frozen eval, with the incumbent measured first as the bar to beat.

**It lost, on one axis, and that axis is the one that decides.** The lab is in
[`../ml/`](../ml); this page is the result.

---

## The bar: the incumbent, measured

`qwen3:8b` through Ollama, 100 frozen eval cases:

| | think = on | **think = off** |
|---|---|---|
| cases scored | 83 (17 timed out at 180 s) | 100 |
| corrections | 71.6% | **79.1%** |
| spurious changes / case | 0.78 | **0.26** |
| exact match | 39.8% | **63.0%** |
| clean text left alone | 1/3 | **5/5** |
| latency median / p95 | 21.1 s / 49.8 s | **3.3 s / 4.6 s** |

**Turning reasoning off improved every axis at once** — more accurate, far more
restrained, 6× faster. Three defects belong to thinking-on specifically:

1. `/think` leaked into the *visible output* in 23 of 83 responses. That string
   would be pasted straight into the document on a FIX press.
2. 17 cases blew a 180 s timeout — three minutes to fix a typo in one sentence.
   They are excluded from the column, so the true think-on numbers are worse
   than shown: it was scored on the easier 83.
3. On `"load-dump lcamp"` (should be `clamp`) it emitted `"load-dump load-dump"`
   — a hallucinated correction, not a missed one.

> This contradicts an earlier planted-typo measurement that had reasoning
> catching 13/13 typos against 4/13 with it off. The two tests are not
> comparable — that one used a long document, this one uses 1–4 sentence
> segments — and Ollama moved a version between them. Treat neither as settled;
> `docs/REWRITE_MENU.md` still carries the older note.

So the target became **79.1% corrections at 0.26 spurious changes per case and
3.3 s** — much harder than the ~17 s figure the project was originally scoped
against, and hard enough that a fast small model is no longer obviously a win.

## The sweep

`Qwen2.5-0.5B-Instruct` and `-1.5B-Instruct`, LoRA (r=16, α=32), 4-bit, 1074
training pairs, identical data and hyperparameters across every run — only the
base model and the epoch count move.

| | ft 0.5B @2ep | ft 0.5B @6ep | ft 1.5B @2ep | **ft 1.5B @6ep** | ft 1.5B @12ep | incumbent |
|---|---|---|---|---|---|---|
| corrections | 64.1% | 69.2% | 78.6% | **82.4%** | 81.3% | 79.1% |
| **spurious / case** | 0.70 | 0.62 | 0.50 | 0.38 | 0.36 | **0.26** |
| exact match | 36.4% | 44.0% | 52.0% | 60.0% | **64.0%** | 63.0% |
| clean left alone | 3/5 | 2/5 | 3/5 | 3/5 | 3/5 | **5/5** |
| latency median | 0.82 s | 0.81 s | 1.85 s | 1.73 s | **1.64 s** | 3.3 s |
| train time | — | 8 min | 7 min | 22 min | 44 min | — |

**The best model in the project is the 1.5B at 6 epochs.** It beats the
incumbent on corrections, ties it on exact match, and is 2× faster.

### Epochs were a real lever, and the default was wrong

The shipped configs carried `epochs: 2  # 3 overfits to the corruption script's
habits`. That comment was an assumption, never measured, and it cost real
performance: going 2 → 6 improved corrections **and** spurious changes together
(+3.8 pts, −0.12), which is not what overfitting looks like on the metric even
though train loss fell to 0.0028.

Doubling again to 12 bought nothing worth 22 extra minutes. **The lever is
exhausted at ~6** — do not escalate further.

### The 0.5B is capacity-limited, not epoch-limited

Epochs move it in the same direction and it never arrives: 10 points behind the
incumbent on corrections and 2.4× its spurious rate. Its only advantage is
0.81 s median latency, and FIX does not need sub-second when the alternative is
1.7 s and considerably more accurate. **The 0.5B is out.**

## Why it still loses

The failure is not missed typos. A word-level diff of prediction against
reference buckets almost everything as **substitution** — 54 of 60 for the 1.5B:
`will`→`may`, `is`→`are`, `sits`→`weighs`, `set a DTC`→`steal DTCs`. Nothing is
truncated. The model is not failing to fix; it is **paraphrasing**.

Per defect class it has plainly learned the mechanical work:

| defect class | incumbent | ft 1.5B |
|---|---|---|
| case | 19/19 | 19/19 |
| dropped / doubled letter | 4/4 | 4/4 |
| punctuation | 32/33 | **33/33** |
| homophone / word choice | 48/60 | 48/60 |
| transposition | **39/52** | 37/52 |
| apostrophe | **1/4** | 0/4 |

So it is competitive at fixing and loses purely on **restraint**.

Three explanations were tested. Two are dead:

- **Loss masking is fine.** Verified directly on a training row — 163 of 192
  tokens masked, and the 29 in-loss tokens decode to exactly the assistant
  response. The alarming `return_assistant_tokens_mask` warning in the log is a
  preferred path falling back to a working one, not a defect.
- **Sampling noise is ruled out.** Every number above is greedy decoding —
  `score_fix.py` calls Ollama at `temperature: 0`.

The surviving explanation is the **training distribution**: 95.6% of the pairs
contain an error, so the model learns that an edit is nearly always required.
Train loss at 0.0009 means it memorised the set and *still* would not hold back,
which says the behaviour is in the data, not in the fitting.

That is the one untested fix and it is a data change, not a training change:
raise the clean-pair share from 4.4% to ~20% **in the train split only**, keep
`fix-eval.jsonl` and its hash frozen, re-run at 6 epochs.

## The verdict

**Keep `qwen3:8b` and set `"think": false` on the FIX style.**

Restraint is the ship criterion. A FIX key that misses a typo is a minor
annoyance; one that quietly rephrases your sentence is a defect you will not
notice until the text is already sent. The incumbent wins there 0.26 to 0.38 and
5/5 to 3/5, and it costs one line of JSON and no GPU.

The margin is narrow enough that this is not a dead end — the fine-tune already
wins on corrections and speed and is one dataset rebalance away from a real
decision. It is just not, today, the better FIX key.

### Caveats, in order of how much they could matter

- **The eval is 100% dense technical documentation prose.** A 6-case hand probe
  at the register FIX actually gets — short notes and messages — had both 6- and
  12-epoch models perfect: clean sentences untouched 3/3, `A4:C1:38:9F:2B:11`
  and `/srv/media/music` preserved, typos corrected to the reference exactly.
  n=6 proves nothing, which is why ~20 real-register snippets are the highest
  value outstanding item. Against that, spurious changes are broken out by input
  length in the runbook and the fine-tunes lose in **every** band including the
  shortest — so "wrong register" does not obviously rescue them.
- **`clean left alone` is n=5.** Far too small to conclude from, either way.
- Both fine-tunes are 4-bit; the bf16 configs never ran (8 GB VRAM — see the
  runbook, a 1.5B + LoRA should fit and does not, because the base loads fp32).
- Export is f16 GGUF rather than q8_0, which is the lower-error choice and so
  does not disadvantage the fine-tunes.
- Latency columns cross machines and Ollama versions. The correction and
  spurious counts are text diffs and are unaffected.

### What this machine is actually running

`~/MacroPadDeck/styles.json` currently names
`soup-macropad-fix-1.5b-4bit-e12:latest` as the **top-level** model, so every
style runs on the fine-tune, not just FIX. That was set to get the rewrite path
working end-to-end when `qwen3:8b` turned out not to be pulled — it predates
this verdict and contradicts it. Change the model back, or keep it knowingly.
