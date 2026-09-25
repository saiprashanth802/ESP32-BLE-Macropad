#!/usr/bin/env python3
"""Score a model on the frozen FIX eval set, via Ollama.

Two numbers, tracked separately and deliberately not averaged together:

  corrections  - of the errors that were actually there, how many did it fix?
  spurious     - how many things did it change that were NOT errors?

A single accuracy figure hides the second one completely, and the second one is
what decides whether this is usable. A model that rewrites clean prose beautifully
is a failure at FIX.

Stdlib only. Needs Ollama running; this is the GPU-touching step.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ENDPOINT = "http://localhost:11434/api/chat"
WORD = re.compile(r"\S+")


def call(model: str, system: str, user: str, think: bool | None,
         timeout: int, num_ctx: int) -> tuple[str, float]:
    body: dict = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
        "stream": False,
        # keep_alive must be a NUMBER; the string "-1" fails every request with
        # 'time: missing unit in duration "-1"'.
        "keep_alive": -1,
        "options": {"temperature": 0, "num_ctx": num_ctx},
    }
    if think is not None:
        body["think"] = think

    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    t0 = time.monotonic()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    elapsed = time.monotonic() - t0
    return payload["message"]["content"].strip(), elapsed


def edits(a: str, b: str) -> set[tuple[int, int, str]]:
    """Word-level edit spans turning `a` into `b`, as a comparable set."""
    aw, bw = WORD.findall(a), WORD.findall(b)
    out = set()
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, aw, bw).get_opcodes():
        if tag != "equal":
            out.add((i1, i2, " ".join(bw[j1:j2])))
    return out


def score_one(src: str, ref: str, pred: str) -> dict:
    true_edits = edits(src, ref)       # the errors that were really there
    pred_edits = edits(src, pred)      # what the model actually changed
    return {
        "exact": pred.strip() == ref.strip(),
        "defects": len(true_edits),
        "corrected": len(true_edits & pred_edits),
        "spurious": len(pred_edits - true_edits),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="Ollama model tag")
    ap.add_argument("--data", type=Path, default=Path("data/fix-eval.jsonl"))
    ap.add_argument("--think", choices=["on", "off"], default=None,
                    help="omit to leave the model's default alone")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--num-ctx", type=int, default=4096)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    rows = [json.loads(l) for l in args.data.open(encoding="utf-8")]
    if args.limit:
        rows = rows[: args.limit]
    think = None if args.think is None else (args.think == "on")

    results, failures = [], 0
    for i, row in enumerate(rows, 1):
        try:
            pred, secs = call(args.model, row["instruction"], row["input"],
                              think, args.timeout, args.num_ctx)
        except (urllib.error.URLError, TimeoutError, KeyError) as exc:
            failures += 1
            print(f"  [{i}/{len(rows)}] request failed: {exc}", file=sys.stderr)
            continue
        s = score_one(row["input"], row["output"], pred)
        s.update(secs=secs, input=row["input"], reference=row["output"], prediction=pred)
        results.append(s)
        print(f"  [{i}/{len(rows)}] {secs:5.1f}s  "
              f"fixed {s['corrected']}/{s['defects']}  spurious {s['spurious']}",
              file=sys.stderr)

    if not results:
        sys.exit("no results — is Ollama running?")

    defects = sum(r["defects"] for r in results)
    corrected = sum(r["corrected"] for r in results)
    spurious = sum(r["spurious"] for r in results)
    exact = sum(r["exact"] for r in results)
    clean_cases = [r for r in results if r["defects"] == 0]
    clean_untouched = sum(1 for r in clean_cases if r["spurious"] == 0)
    secs = sorted(r["secs"] for r in results)

    print(f"\nmodel            {args.model}  (think={args.think or 'default'})")
    print(f"cases            {len(results)}  ({failures} failed)")
    print(f"corrections      {corrected}/{defects}"
          f"  ({100 * corrected / defects:.1f}%)" if defects else "")
    print(f"spurious changes {spurious}  ({spurious / len(results):.2f} per case)")
    print(f"exact match      {exact}/{len(results)}"
          f"  ({100 * exact / len(results):.1f}%)")
    if clean_cases:
        print(f"clean left alone {clean_untouched}/{len(clean_cases)}")
    print(f"latency          median {secs[len(secs) // 2]:.2f}s   "
          f"p95 {secs[int(0.95 * (len(secs) - 1))]:.2f}s")

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(results, indent=2, ensure_ascii=False),
                            encoding="utf-8")
        print(f"\nper-case detail -> {args.out}")


if __name__ == "__main__":
    main()
