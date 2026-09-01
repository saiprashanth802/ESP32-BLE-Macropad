# `ml/` — the FIX fine-tune lab

The pad's rewrite menu has a **FIX** style: select text, press the key, get the
spelling and grammar corrected in place and nothing else touched. It runs on a
local model. This directory is the experiment that asked whether a small
fine-tuned model should replace the general-purpose one, and answered it with
numbers.

**Verdict: it should not — keep `qwen3:8b` with `"think": false`.** The full
argument and every measurement is in [`../docs/FIX_MODEL_FINETUNE.md`](../docs/FIX_MODEL_FINETUNE.md).
Read that first; this file is only how to re-run the work.

---

## What is here

| | |
|---|---|
| `make_fix_data.py` | Builds the train/eval pairs — takes clean sentences and corrupts them, deterministically |
| `check_data.py` | Pre-flight: leakage between splits, near-duplicate overlap, clean-pair share |
| `score_fix.py` | Scores any Ollama model against the frozen eval — corrections, spurious changes, exact match, latency |
| `build_real_eval.py` | Builds a second eval from hand-written real-register snippets |
| `configs/` | Soup training configs — the sweep that was actually run |
| `RUNBOOK.md` | The raw lab record: every command, every result table, every dead end |
| `HANDOFF.md` | The problem statement and design rationale written before the sweep |

## What is deliberately not here

**The corpus, the datasets, the scored results and the model weights.** The
training pairs were generated from the author's private hardware-handoff notes,
so `data/`, `results/`, `output/` and `corpus/` are gitignored. The generator is
committed and deterministic, which is the part that matters: give it your own
documents and it rebuilds an equivalent set.

The exported GGUFs are ~1–3 GB each and belong nowhere near a git repo.

## Rebuilding the data

Any directory tree of `.md` files works as a corpus. Prose beats reference
tables — the generator mines sentences, so bullet soup gives it little to work
with.

```bash
cd ml
python make_fix_data.py --docs-root ~/my-notes --docs-root ~/more-notes
python check_data.py
```

Defaults: 6 documents held out **whole** for the eval split (never sentence-wise
— an eval sentence must not have a near-twin in training), 3 corrupted variants
per clean segment, 5% clean pairs, `seed=20260816`. Same inputs give the same
files.

`check_data.py` is the go/no-go. It fails on exact leakage and reports the worst
near-duplicate overlap; the original run passed at 14%.

> **Freeze the eval before you look at a single result.** `data/fix-eval.jsonl`
> was pinned at `cdbfa3a0…` and every number in the runbook is against that
> hash. If the eval changes after results have been seen, the scoreboard is void
> — regenerate everything or change nothing.

## Training

The training stack needs Python 3.12 and a CUDA GPU. On 8 GB you are limited to
4-bit; see the VRAM section of `RUNBOOK.md` for why bf16 does not fit a 1.5B
here despite the arithmetic suggesting it should.

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
uv venv --python 3.12 .venv && source .venv/bin/activate
uv pip install soup-cli
uv pip install "soup-cli[train]"
soup doctor                                   # only meaningful after [train]
soup train --config configs/probe-1.5b-4bit-e6.yaml --yes
```

`--yes` is not optional in a non-interactive shell. Without it `soup train`
prints `Start training? [Y/n]: Aborted.` and **exits 0**, so a backgrounded run
looks like it started and silently did nothing.

`configs/probe-1.5b-4bit-e6.yaml` is the best model this project produced.
`soup-0.5b.yaml` / `soup-1.5b.yaml` are the original pair and the 1.5B one does
not run on 8 GB — they are kept because the runbook's early tables refer to them.

## Scoring

`score_fix.py` talks to Ollama at `temperature: 0`, so runs are greedy and
repeatable.

```bash
python score_fix.py --model qwen3:8b --think off --out results/baseline.json
python score_fix.py --model soup-macropad-fix-1.5b-4bit-e6:latest --out results/ft-e6.json
```

It reports four things. Only one of them decides anything:

- **corrections** — of the errors that were planted, how many came back fixed
- **spurious changes** — edits made that the reference did not ask for. **This is
  the ship criterion.** A FIX key that quietly rephrases your sentence is worse
  than one that misses a typo, because you will not notice
- **exact match** — whole output identical to the reference
- **latency** — median and p95, per case

## Using a fine-tune anyway

Nothing stops you. Export to GGUF, register it with Ollama, and name it in the
FIX entry of `~/MacroPadDeck/styles.json` (`%USERPROFILE%\MacroPadDeck\` on
Windows) — that file hot-reloads, so the pad picks it up in under a second with
no rebuild. See [`../docs/REWRITE_MENU.md`](../docs/REWRITE_MENU.md).

Set `"think": false` on the FIX style whichever model you use. That one line was
worth more than the entire training sweep.
