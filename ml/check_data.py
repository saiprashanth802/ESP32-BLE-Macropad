#!/usr/bin/env python3
"""Pre-flight checks on the FIX dataset. Cheap, deterministic, no GPU.

Run this before every training run. The failure it exists to catch is leakage:
an eval sentence that also appears in training makes the scoreboard a lie, and
near-duplicates are the sneaky way it happens.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

DATA = Path("data")
NORM = re.compile(r"\W+")


def norm(s: str) -> str:
    return NORM.sub("", s.lower())


def shingles(s: str, k: int = 5) -> set[str]:
    w = s.lower().split()
    return {" ".join(w[i : i + k]) for i in range(max(0, len(w) - k + 1))}


def load(name: str) -> list[dict]:
    rows = []
    path = DATA / name
    for i, line in enumerate(path.open(encoding="utf-8"), 1):
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError as exc:
            sys.exit(f"FAIL {path}:{i} does not parse — {exc}")
    return rows


def main() -> None:
    train = load("fix-train.jsonl")
    evals = load("fix-eval.jsonl")
    ok = True

    print(f"train {len(train)} rows   eval {len(evals)} rows   [all parse]")

    # --- schema ---------------------------------------------------------
    for name, rows in (("train", train), ("eval", evals)):
        bad = [r for r in rows if set(r) != {"instruction", "input", "output"}]
        if bad:
            print(f"FAIL {name}: {len(bad)} rows with wrong keys")
            ok = False
    instructions = {r["instruction"] for r in train} | {r["instruction"] for r in evals}
    if len(instructions) != 1:
        print(f"FAIL instruction is not constant ({len(instructions)} variants)")
        ok = False
    else:
        print("instruction      constant across all rows")

    # --- exact leakage --------------------------------------------------
    train_refs = {norm(r["output"]) for r in train}
    train_ins = {norm(r["input"]) for r in train}
    exact = [r for r in evals if norm(r["output"]) in train_refs
             or norm(r["input"]) in train_ins]
    if exact:
        print(f"FAIL {len(exact)} eval rows appear verbatim in training")
        ok = False
    else:
        print("exact leakage    none")

    # --- near-duplicate leakage ----------------------------------------
    train_sh: set[str] = set()
    for r in train:
        train_sh |= shingles(r["output"])
    worst = 0.0
    near = 0
    for r in evals:
        sh = shingles(r["output"])
        if not sh:
            continue
        overlap = len(sh & train_sh) / len(sh)
        worst = max(worst, overlap)
        if overlap > 0.5:
            near += 1
    if near:
        print(f"FAIL {near} eval rows share >50% of 5-word shingles with training")
        ok = False
    else:
        print(f"near-duplicates  none (worst overlap {worst:.0%})")

    # --- composition ----------------------------------------------------
    clean = sum(1 for r in train if r["input"] == r["output"])
    share = clean / len(train)
    verdict = "ok" if 0.02 <= share <= 0.10 else "OUT OF RANGE"
    print(f"clean pairs      {clean}/{len(train)} = {share:.1%}  [{verdict}]")
    if verdict != "ok":
        ok = False

    lens = sorted(len(r["input"]) for r in train)
    print(f"input length     median {lens[len(lens) // 2]}  "
          f"min {lens[0]}  max {lens[-1]} chars")

    dupes = len(train) - len({(r["input"], r["output"]) for r in train})
    print(f"duplicate pairs  {dupes}")

    # --- freeze ---------------------------------------------------------
    h = hashlib.sha256((DATA / "fix-eval.jsonl").read_bytes()).hexdigest()
    print(f"\neval sha256      {h[:16]}…")
    print("                 record this — if it changes after you have seen")
    print("                 results, the scoreboard no longer means anything.")

    print("\nPASS" if ok else "\nFAIL — fix the above before training")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
