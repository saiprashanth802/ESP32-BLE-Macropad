#!/usr/bin/env python3
"""Build the FIX training set: take clean prose, corrupt it, emit (corrupted -> clean) pairs.

Stdlib only, deterministic. Ground truth is free because we start from the answer.

The corruption classes below are modelled on typos people actually make. Random
character noise would teach the model to fix a distribution that does not exist.
"""

from __future__ import annotations

import argparse
import json
import random
import re
from dataclasses import dataclass
from pathlib import Path

# The FIX system prompt from styles.json, verbatim. The trained model and the app
# have to agree on what the job is.
INSTRUCTION = (
    "Correct every spelling mistake, typo, grammatical error, and punctuation error "
    "in the user's text. Fix capitalisation — including the pronoun \"I\" and the "
    "first word of every sentence — and add missing sentence-ending punctuation. "
    "Keep the author's exact wording, word order, tone and line breaks: do not "
    "rephrase, reorder, merge or split sentences, and do not make the text more "
    "formal. Use only facts present in the input. Never invent names, dates, times, "
    "numbers, or events. Output only the resulting text. No preamble, no commentary, "
    "no explanation, and do not wrap the output in quotation marks."
)

# ---------------------------------------------------------------- extraction

FRONTMATTER = re.compile(r"\A---\n.*?\n---\n", re.S)
FENCE = re.compile(r"```.*?```", re.S)
HEADING = re.compile(r"^\s{0,3}#{1,6}\s.*$", re.M)
TABLE_ROW = re.compile(r"^\s*\|.*$", re.M)
HTML = re.compile(r"<[^>]+>")
MD_LINK = re.compile(r"\[([^\]]+)\]\([^)]*\)")
WIKILINK = re.compile(r"\[\[([^\]]+)\]\]")
LIST_MARKER = re.compile(r"^\s*(?:[-*+]|\d+\.)\s+", re.M)
BLOCKQUOTE = re.compile(r"^\s*>\s?", re.M)
URL = re.compile(r"\b(?:https?://|www\.)\S+")
WINPATH = re.compile(r"[A-Za-z]:\\\\?[\w\\/.~-]+")

# A "protected" token must survive corruption untouched: inline code, identifiers,
# hex/MAC addresses, versions, anything with digits or underscores or CamelCase.
PROTECTED = re.compile(
    r"`[^`]*`"                      # inline code
    r"|\b[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){3,}\b"   # MAC
    r"|\b0x[0-9A-Fa-f]+\b"          # hex literal
    r"|\S*[_/\\]\S*"                # paths, snake_case
    r"|\S*\d\S*"                    # anything containing a digit
    r"|\b[a-z]+[A-Z]\w*\b"          # camelCase
    r"|\b[A-Z]{2,}\b"               # acronyms
)

SENTENCE_END = re.compile(r"(?<=[.!?])[ \n]+(?=[A-Z\"'(])")


def clean_markdown(text: str) -> str:
    text = FRONTMATTER.sub("", text)
    text = FENCE.sub(" ", text)
    text = HEADING.sub(" ", text)
    text = TABLE_ROW.sub(" ", text)
    text = HTML.sub(" ", text)
    text = MD_LINK.sub(r"\1", text)
    text = WIKILINK.sub(r"\1", text)
    text = BLOCKQUOTE.sub("", text)
    text = LIST_MARKER.sub("", text)
    text = URL.sub(" ", text)
    text = WINPATH.sub(" ", text)
    text = re.sub(r"\*\*|__|~~", "", text)          # bold / strike
    # Single-asterisk emphasis. Underscores are left alone — snake_case.
    text = re.sub(r"\*(\S(?:[^*\n]*\S)?)\*", r"\1", text)
    text = re.sub(r"[ \t]+", " ", text)
    return text


def sentences(text: str) -> list[str]:
    out: list[str] = []
    for para in re.split(r"\n\s*\n", text):
        para = para.replace("\n", " ").strip()
        if not para:
            continue
        for s in SENTENCE_END.split(para):
            s = s.strip()
            if is_usable(s):
                out.append(s)
    return out


DOUBLED_WORD = re.compile(r"\b(the|a|to|of|is|in|and|that|for)\s+\1\b", re.I)


def label_is_clean(s: str) -> bool:
    """The output side is ground truth — it has to actually be correct.

    Source prose is human-written and occasionally already has the exact defects
    we are about to inject. Training on those teaches the model to preserve them.
    """
    if DOUBLED_WORD.search(s):
        return False
    if "  " in s:
        return False
    if re.search(r"\s[,.;:]", s):        # space before punctuation
        return False
    if re.search(r"\bi\b", s):           # bare lowercase pronoun
        return False
    return True


def is_usable(s: str) -> bool:
    if not (25 <= len(s) <= 220):
        return False
    if not s[0].isupper():
        return False
    if s[-1] not in ".!?":
        return False
    words = s.split()
    if len(words) < 5:
        return False
    # Reject sentences that are mostly identifiers/numbers — they teach nothing
    # about English and everything about not touching code.
    protected_chars = sum(len(m.group()) for m in PROTECTED.finditer(s))
    if protected_chars > 0.35 * len(s):
        return False
    return True


# ---------------------------------------------------------------- corruption

HOMOPHONES = {
    "their": "there", "there": "their", "they're": "their",
    "its": "it's", "it's": "its",
    "your": "you're", "you're": "your",
    "then": "than", "than": "then",
    "to": "too", "too": "to",
    "affect": "effect", "effect": "affect",
    "lose": "loose", "loose": "lose",
    "whose": "who's", "who's": "whose",
}

VOWELS = "aeiou"


@dataclass
class Span:
    start: int
    end: int


def protected_spans(text: str) -> list[Span]:
    return [Span(m.start(), m.end()) for m in PROTECTED.finditer(text)]


def word_positions(text: str, protected: list[Span]) -> list[Span]:
    """Word spans that do not overlap anything protected."""
    out = []
    for m in re.finditer(r"[A-Za-z']+", text):
        if any(m.start() < p.end and p.start < m.end() for p in protected):
            continue
        out.append(Span(m.start(), m.end()))
    return out


def _replace(text: str, span: Span, new: str) -> str:
    return text[: span.start] + new + text[span.end :]


def c_transpose(text: str, rng: random.Random) -> str | None:
    """teh, adn — swap two adjacent characters inside a word."""
    cands = [w for w in word_positions(text, protected_spans(text)) if w.end - w.start >= 3]
    rng.shuffle(cands)
    for w in cands:
        word = text[w.start : w.end]
        i = rng.randrange(len(word) - 1)
        if word[i] == word[i + 1]:
            continue
        return _replace(text, w, word[:i] + word[i + 1] + word[i] + word[i + 2 :])
    return None


def c_drop_letter(text: str, rng: random.Random) -> str | None:
    """comitted — drop one letter of a doubled pair, or any interior letter."""
    cands = [w for w in word_positions(text, protected_spans(text)) if w.end - w.start >= 4]
    rng.shuffle(cands)
    for w in cands:
        word = text[w.start : w.end]
        doubles = [i for i in range(len(word) - 1) if word[i] == word[i + 1]]
        if doubles:
            i = rng.choice(doubles)
            return _replace(text, w, word[:i] + word[i + 1 :])
        i = rng.randrange(1, len(word) - 1)
        return _replace(text, w, word[:i] + word[i + 1 :])
    return None


def c_double_letter(text: str, rng: random.Random) -> str | None:
    """sucessfull the other way — duplicate an interior letter."""
    cands = [w for w in word_positions(text, protected_spans(text)) if w.end - w.start >= 4]
    rng.shuffle(cands)
    for w in cands:
        word = text[w.start : w.end]
        i = rng.randrange(1, len(word) - 1)
        return _replace(text, w, word[: i + 1] + word[i] + word[i + 1 :])
    return None


def c_lowercase_start(text: str, rng: random.Random) -> str | None:
    """Lowercase the first word of the segment."""
    if text and text[0].isupper():
        return text[0].lower() + text[1:]
    return None


def c_lowercase_i(text: str, rng: random.Random) -> str | None:
    """I -> i, the classic."""
    hits = [m for m in re.finditer(r"\bI\b", text)]
    if not hits:
        return None
    m = rng.choice(hits)
    return text[: m.start()] + "i" + text[m.end() :]


def c_drop_terminal(text: str, rng: random.Random) -> str | None:
    if text and text[-1] in ".!?":
        return text[:-1]
    return None


def c_drop_apostrophe(text: str, rng: random.Random) -> str | None:
    hits = [m for m in re.finditer(r"\b\w+'(?:s|t|re|ve|ll|d|m)\b", text)]
    if not hits:
        return None
    m = rng.choice(hits)
    return text[: m.start()] + m.group().replace("'", "") + text[m.end() :]


def c_homophone(text: str, rng: random.Random) -> str | None:
    protected = protected_spans(text)
    cands = []
    for m in re.finditer(r"\b[A-Za-z']+\b", text):
        if any(m.start() < p.end and p.start < m.end() for p in protected):
            continue
        if m.group().lower() in HOMOPHONES:
            cands.append(m)
    if not cands:
        return None
    m = rng.choice(cands)
    swap = HOMOPHONES[m.group().lower()]
    if m.group()[0].isupper():
        swap = swap.capitalize()
    return text[: m.start()] + swap + text[m.end() :]


def c_double_word(text: str, rng: random.Random) -> str | None:
    """the the — duplicate a short function word."""
    hits = [m for m in re.finditer(r"\b(the|a|to|of|is|in|and|that|for)\b", text)]
    if not hits:
        return None
    m = rng.choice(hits)
    return text[: m.end()] + " " + m.group() + text[m.end() :]


# (function, weight) — weights are the shares from the plan, normalised.
CORRUPTIONS = [
    (c_transpose, 20),
    (c_drop_letter, 12),
    (c_double_letter, 8),
    (c_lowercase_start, 8),
    (c_lowercase_i, 7),
    (c_drop_terminal, 15),
    (c_drop_apostrophe, 10),
    (c_homophone, 10),
    (c_double_word, 5),
]
FUNCS = [f for f, _ in CORRUPTIONS]
WEIGHTS = [w for _, w in CORRUPTIONS]


def corrupt(clean: str, rng: random.Random, n: int) -> str:
    text = clean
    applied = 0
    for _ in range(n * 4):          # retries: some classes don't apply to some text
        if applied >= n:
            break
        fn = rng.choices(FUNCS, weights=WEIGHTS, k=1)[0]
        out = fn(text, rng)
        if out is not None and out != text:
            text = out
            applied += 1
    return text


# ---------------------------------------------------------------- assembly

def gather(paths: list[Path]) -> dict[str, list[str]]:
    """doc-name -> sentences, deduped globally (first occurrence wins)."""
    seen: set[str] = set()
    docs: dict[str, list[str]] = {}
    for p in sorted(paths):
        try:
            raw = p.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        keep = []
        for s in sentences(clean_markdown(raw)):
            key = re.sub(r"\W+", "", s.lower())
            if key in seen:
                continue          # mirrored file — already claimed by another doc
            seen.add(key)
            keep.append(s)
        if keep:
            docs[str(p)] = keep
    return docs


def shingles(s: str, k: int = 5) -> set[str]:
    w = s.lower().split()
    return {" ".join(w[i : i + k]) for i in range(max(0, len(w) - k + 1))}


def drop_leaks(eval_segs: list[str], train_segs: list[str],
               threshold: float = 0.3) -> list[str]:
    """Remove eval segments that overlap training text.

    Splitting by document is not sufficient on this corpus: the memory files are
    mirrors of the handoff docs and the two copies drift apart by a few words, so
    exact dedup lets near-twins through. Those must not be scored on.
    """
    train_sh: set[str] = set()
    for s in train_segs:
        train_sh |= shingles(s)
    keep = []
    for s in eval_segs:
        sh = shingles(s)
        if sh and len(sh & train_sh) / len(sh) > threshold:
            continue
        keep.append(s)
    return keep


def segments(sents: list[str], rng: random.Random) -> list[str]:
    """Group 1-4 consecutive sentences, matching what you'd select on screen."""
    out, i = [], 0
    while i < len(sents):
        n = rng.choices([1, 2, 3, 4], weights=[45, 30, 17, 8], k=1)[0]
        chunk = re.sub(r" {2,}", " ", " ".join(sents[i : i + n])).strip()
        if 30 <= len(chunk) <= 450 and label_is_clean(chunk):
            out.append(chunk)
        i += n
    return out


def build(segs: list[str], rng: random.Random, clean_share: float,
          variants: int) -> list[dict]:
    """`variants` differently-corrupted rows per clean segment.

    The corpus is fixed and small; the corruptions are not. Capped low so the
    model does not over-see any single clean output and start memorising it.
    """
    rows, seen = [], set()
    for seg in segs:
        for _ in range(variants):
            if rng.random() < clean_share:
                # Already-correct text. Without these the model learns there is
                # always something to fix, and starts rephrasing clean input.
                bad = seg
            else:
                n = rng.choices([1, 2, 3, 4], weights=[40, 32, 19, 9], k=1)[0]
                bad = corrupt(seg, rng, n)
                if bad == seg:
                    continue            # nothing applied; drop rather than mislabel
            key = (bad, seg)
            if key in seen:
                continue
            seen.add(key)
            rows.append({"instruction": INSTRUCTION, "input": bad, "output": seg})
    return rows


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data", type=Path)
    ap.add_argument("--eval-docs", type=int, default=6,
                    help="documents held out entirely for the eval set")
    ap.add_argument("--clean-share", type=float, default=0.05)
    ap.add_argument("--variants", type=int, default=3,
                    help="corrupted variants per clean segment (volume lever)")
    ap.add_argument("--seed", type=int, default=20260816)
    ap.add_argument("--docs-root", type=Path, action="append",
                    help="directory of .md files to mine for sentences; "
                         "repeatable. Default: ./corpus")
    args = ap.parse_args()

    roots = args.docs_root or [Path("corpus")]
    missing = [r for r in roots if not r.exists()]
    if len(missing) == len(roots):
        ap.error(
            "no corpus found in " + ", ".join(str(r) for r in roots) +
            ". Point --docs-root at one or more directories of your own .md "
            "files; see ml/README.md. The corpus is never committed."
        )
    paths = [p for root in roots if root.exists() for p in root.rglob("*.md")
             if ".git" not in p.parts]

    rng = random.Random(args.seed)
    docs = gather(paths)
    names = sorted(docs)
    rng.shuffle(names)

    # Split by DOCUMENT, not by sentence: an eval sentence must never have a
    # near-twin in training.
    eval_names = names[: args.eval_docs]
    train_names = names[args.eval_docs :]

    train_segs = [s for n in train_names for s in segments(docs[n], rng)]
    eval_segs = [s for n in eval_names for s in segments(docs[n], rng)]
    before = len(eval_segs)
    eval_segs = drop_leaks(eval_segs, train_segs)
    if before != len(eval_segs):
        print(f"dropped {before - len(eval_segs)} eval segments overlapping training")
    rng.shuffle(train_segs)
    rng.shuffle(eval_segs)

    train = build(train_segs, rng, args.clean_share, args.variants)
    # One variant per eval segment — the scoreboard wants breadth, not repeats.
    evals = build(eval_segs, rng, args.clean_share, 1)[:100]

    args.out.mkdir(parents=True, exist_ok=True)
    for name, rows in (("fix-train.jsonl", train), ("fix-eval.jsonl", evals)):
        path = args.out / name
        with path.open("w", encoding="utf-8") as fh:
            for r in rows:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
        # Validate: every line must parse, or training dies 40 minutes in.
        with path.open(encoding="utf-8") as fh:
            for i, line in enumerate(fh, 1):
                json.loads(line)
        print(f"{path}: {len(rows)} rows, all parse")

    print(f"\ndocs: {len(docs)}  train docs: {len(train_names)}  eval docs: {len(eval_names)}")
    print("held out:", ", ".join(Path(n).name for n in eval_names))


if __name__ == "__main__":
    main()
