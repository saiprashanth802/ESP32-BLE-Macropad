#!/usr/bin/env python3
"""Build data/fix-eval-real.jsonl from hand-written real-register snippets.

The synthetic eval is 100% documentation prose. Real FIX presses are emails,
messages and notes — a different register, and the scoreboard is weaker without
them. This turns pasted snippets into the same JSONL schema score_fix.py reads,
so the real set is scored by exactly the same harness:

    python build_real_eval.py real_snippets.txt
    python score_fix.py --model <tag> --data data/fix-eval-real.jsonl \
                        --out results/<tag>-real.json

INPUT FORMAT — plain text, one snippet per block, blocks separated by a blank
line. Each block is exactly two lines:

    line 1: the text AS TYPED (with the typos)
    line 2: the text AS IT SHOULD BE

Example:

    hey can you send me teh invoice before friday
    Hey, can you send me the invoice before Friday?

    I pushed the fix to main, its running on the board now
    I pushed the fix to main, it's running on the board now.

Notes:
- Do NOT paraphrase in line 2. It is the reference: it should differ from line 1
  ONLY by the corrections. Any rewording you do here is scored as a defect the
  model failed to reproduce, and quietly makes every model look worse.
- Real typos beat invented ones. Text you actually sent, mistakes included, is
  the whole point of this file.
- The instruction string is copied verbatim from the frozen synthetic eval so
  the two sets are directly comparable. It is never written by hand here.
- This file is SEPARATE from data/fix-eval.jsonl. The frozen eval's hash
  (cdbfa3a074cff0a3...) is untouched, so the existing scoreboard stays valid.

Stdlib only, no network, deterministic.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

FROZEN_EVAL = Path("data/fix-eval.jsonl")
DEFAULT_OUT = Path("data/fix-eval-real.jsonl")


def load_instruction(frozen: Path) -> str:
    """Take the instruction verbatim from the frozen eval — never retype it."""
    if not frozen.exists():
        sys.exit(f"error: {frozen} not found; run from the soup-lab directory")
    with frozen.open(encoding="utf-8") as fh:
        first = json.loads(fh.readline())
    instr = first.get("instruction")
    if not instr:
        sys.exit(f"error: no 'instruction' field in {frozen}")
    return instr


def parse_blocks(text: str) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for n, block in enumerate(text.split("\n\n"), start=1):
        lines = [ln for ln in block.splitlines() if ln.strip()]
        if not lines:
            continue
        if len(lines) != 2:
            sys.exit(
                f"error: block {n} has {len(lines)} non-empty line(s), expected 2 "
                f"(typed, then corrected).\n  offending block: {lines[:3]}"
            )
        typed, correct = lines[0].rstrip(), lines[1].rstrip()
        if typed == correct:
            print(f"  note: block {n} is a clean pair (no corrections) — "
                  f"these are valuable, keep some")
        pairs.append((typed, correct))
    return pairs


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("snippets", type=Path,
                    help="plain-text file, blank-line-separated 2-line blocks")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--frozen", type=Path, default=FROZEN_EVAL)
    args = ap.parse_args()

    if not args.snippets.exists():
        sys.exit(f"error: {args.snippets} not found")

    instruction = load_instruction(args.frozen)
    pairs = parse_blocks(args.snippets.read_text(encoding="utf-8"))
    if not pairs:
        sys.exit("error: no snippet blocks found")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as fh:
        for typed, correct in pairs:
            fh.write(json.dumps({
                "instruction": instruction,
                "input": typed,
                "output": correct,
            }, ensure_ascii=False) + "\n")

    digest = hashlib.sha256(args.out.read_bytes()).hexdigest()
    clean = sum(1 for t, c in pairs if t == c)
    lengths = sorted(len(t) for t, _ in pairs)
    print(f"\nwrote {args.out}  —  {len(pairs)} rows")
    print(f"clean pairs      {clean}/{len(pairs)}")
    print(f"input length     median {lengths[len(lengths) // 2]}  "
          f"min {lengths[0]}  max {lengths[-1]}")
    print(f"sha256           {digest[:16]}...")
    print("\nRecord that hash. If it changes after you have seen results,")
    print("the real-register scoreboard no longer means anything either.")
    print(f"\nnext: python score_fix.py --model <tag> --data {args.out} "
          f"--out results/<tag>-real.json")


if __name__ == "__main__":
    main()
