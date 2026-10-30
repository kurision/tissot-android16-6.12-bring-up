#!/usr/bin/env python3
"""Collapse large kernel boot logs into an AI-analysis-friendly digest.

Kernel/boot logs repeat themselves enormously: hundreds of identical audit
rate-limit lines, uevent dumps, and boot memmaps crowd out the few WARN/ERROR
lines that actually explain a broken boot. This tool rewrites a raw log into
a digest that keeps all of the signal (severity >= WARN, oops/panic/call
traces, driver failures) while bounding everything else.

Guarantees for analysis:
  * Input files are never modified; the digest is a new file.
  * No line is ever deleted silently -- repeats become counted markers, and
    every dropped/truncated chunk is accounted for in the stats header.
  * Patterns nobody has seen before ("new symbols") are never dropped: they
    collapse to a bounded number of occurrences with an exact xN count, and
    --suggest ranks them so frequent ones can be promoted into a --rules file.

Usage:
  scripts/logdigest.py <log> [<log> ...] [-o OUT] [--budget KB]
                       [--golden FILE] [--rules FILE] [--cross-dedup] [--suggest]

Exit status is 0 on success, 2 if keep-always lines could not fit the budget.
"""
import argparse
import re
import sys

DEDUP_OCCURRENCES = 3  # first / middle / last shown for repeats
DEFAULT_BUDGET = 200   # KB
KEEP_SEV = 4           # syslog warning or worse is always kept

LOGDUMP_RE = re.compile(r"^(\d+),(\d+),(\d+),-(?:,[^;]*)?;(.*)$")
DMESG_RE = re.compile(r"^<(\d+)>\[([^\]]*)\]\s*(.*)$")

# Keep-always: never collapsed to nothing, never dropped by budget.
# High-precision only -- a false negative here loses the actual bug.
KEEP_PATTERNS = (
    r"\bBUG\b:",
    r" Unable to handle kernel",
    r"Kernel panic",
    r"Call trace:?",
    r"\bOops\b",
    r"Internal error:",
    r"\bKASAN\b",
    r"segfault at ",
    r"PC is at ",
    r"LR is at ",
    r"WARNING: CPU|WARNING: at |W WARNING:",
    r"W BUG:",
    r"\bSError\b",
    r"watchdog: BUG",
    r"soft lockup|hard LOCKUP",
    r"rcu_sched (?:self-)?detected",
    r"schedule while atomic",
    r"list_add (?:single )?corruption",
    r"refcount_t: .*(over|underflow)",
    r"UBSan:",
    r"end Kernel panic",
    r"Failed to (?:mount|load|start|probe)",
    r"unable to mount root fs",
    r"\bKilled process\b",
)

# Known-noise: repeated boot lines with no analytic value. Extend via --rules;
# anything not listed is never dropped, only bounded (see TestUnknownSymbols).
NOISE_PATTERNS = (
    r"^audit: rate limit exceeded$",
    r"^audit: audit_lost=",
    r"^audit: (audit_)?backlog",  # 'avc: denied' lines deliberately NOT noise
    r"^SUBSYSTEM=",
    r"^DEVICE=",
    r"^node\s+\d+: \[mem ",
    r"^OF: reserved mem:",
    r"Skipping entries from slow reader",
    r"^init: Parsing file .*\.rc",
    r"^Freeing unused kernel (image|memory)",
    r"^devtmpfs: ",
    r"^clocksource: ",
    r"^percpu: Embedded",
    r"^Built \d+ zonelists",
    r"^Memory: ",
    r"^smp: Bringing up secondary CPUs",
    r"^KASLR disabled",
    r"^Machine model: ",
    r"^Reserved memory: ",
    r"^SLUB: ",
    r"^Brought up \d+ node",
    r"^Run /",
    r"^kworker/u\d+.*took \d+ usecs",
)

CLASS_KEEP, CLASS_REST, CLASS_NOISE = "keep", "rest", "noise"
CLASS_PRECEDENCE = {CLASS_KEEP: 0, CLASS_REST: 1, CLASS_NOISE: 2}
BUDGET_MARKER_RESERVE = 80  # bytes left for [...] markers inside budgets


class Entry:
    """One parsed log line, in its original (verbatim) form."""

    __slots__ = ("msg", "sev", "kern_prio", "ts", "key", "lineno")

    def __init__(self, msg, sev=None, kern_prio=None, ts=None):
        self.msg = msg
        self.sev = sev          # syslog 0..7 (0 most severe), or None
        self.kern_prio = kern_prio
        self.ts = ts
        self.key = None
        self.lineno = 0


def parse_line(line):
    """Split one raw log line into an Entry, or None for blank lines.

    Handles the shapes found in Android/GKI trees: logd CSV dumps
    (``prio,seq,pid,-,caller=Tn;msg`` and ``prio,seq,pid,-;msg``), kernel dmesg
    (``<14>[    7.407159] msg``) and plain serial lines.
    """
    text = line.rstrip("\n")
    if not text.strip():
        return None
    m = LOGDUMP_RE.match(text)
    if m:
        prio = int(m.group(1))
        return Entry(m.group(4).strip(), sev=prio if 0 <= prio <= 7 else None)
    m = DMESG_RE.match(text)
    if m:
        return Entry(m.group(3).strip(), sev=int(m.group(1)) & 7,
                     kern_prio=int(m.group(1)), ts=m.group(2).strip())
    return Entry(text.strip())


_HEXISH_RE = re.compile(r"\b(?=[0-9a-f]*\d)[0-9a-f]{5,}\b")
_LONGNUM_RE = re.compile(r"\b\d{4,}\b")
_WS_RE = re.compile(r"\s+")


def group_key(entry):
    """Normalized identity of a message: same key means same event.

    Strips addresses, PIDs and long numbers; instance suffixes and short ids
    survive (``q6asm-fe.0`` is distinct from ``q6asm-fe.1``). The verbatim
    message in ``entry.msg`` is what gets printed.
    """
    if entry.key is None:
        k = entry.msg.lower()
        k = re.sub(r"0x[0-9a-f]+", "0xADDR", k)
        k = _HEXISH_RE.sub("HEXADDR", k)
        k = _LONGNUM_RE.sub("N", k)
        entry.key = _WS_RE.sub(" ", k).strip()
    return entry.key


class Rules:
    """Pattern sets for classification. Extensible without code changes."""

    def __init__(self, keep=(), noise=()):
        self.keep = tuple(re.compile(p) for p in keep)
        self.noise = tuple(re.compile(p) for p in noise)

    @classmethod
    def default(cls):
        return cls(KEEP_PATTERNS, NOISE_PATTERNS)

    @classmethod
    def load(cls, path):
        """Load a rules file: lines of 'noise: regex' or 'keep: regex'."""
        keep, noise = [], []
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                kind, _, pattern = line.partition(":")
                kind, pattern = kind.strip(), pattern.strip()
                if kind == "noise":
                    noise.append(pattern)
                elif kind == "keep":
                    keep.append(pattern)
        return cls(keep, noise)

    def merged(self, extra):
        return Rules(keep=tuple(self.keep) + tuple(extra.keep),
                     noise=tuple(self.noise) + tuple(extra.noise))

    def noise_family(self, entry):
        """The noise pattern matching this line, or None if not noise.

        Curated noise patterns outrank severity: 'audit: rate limit exceeded'
        is printed at KERN_ERR yet carries no information -- that is precisely
        what declaring it noise means. Content keep-patterns always win over
        noise patterns, so opt-in rules can never hide a BUG/oops.
        """
        if any(r.search(entry.msg) for r in self.keep):
            return None
        for p in self.noise:
            if p.search(entry.msg):
                return p.pattern
        return None

    def classify(self, entry):
        if self.noise_family(entry) is not None:
            return CLASS_NOISE
        if any(r.search(entry.msg) for r in self.keep):
            return CLASS_KEEP
        if entry.sev is not None and entry.sev <= KEEP_SEV:
            return CLASS_KEEP  # warning or worse, anything not declared noise
        return CLASS_REST


class Digest(str):
    """The digest text, carrying its stats dict and overflow flag."""

    stats = {}
    overflow = False


def digest(raw_lines, rules=None, golden_keys=None, budget_bytes=None,
           cross_keys=None):
    """Rewrite raw log lines into a signal-dense, countable digest string.

    Unknown patterns are never dropped: a repeat collapses to its first (noise),
    or first/middle/last (everything else) occurrence plus an exact xN count.

    ``cross_keys`` is a dict shared between digest() calls on sibling logs of
    the same boot: keys already emitted there are dropped here and counted in
    stats["cross_dropped"], so N boots of one kernel cost ~1 boot plus deltas.
    """
    rules = rules or Rules.default()
    groups = {}
    stats = {"input": 0, "output": 0, "golden_dropped": 0, "cross_dropped": 0,
             "blank": 0, "collapsed": 0, "elided": 0,
             "classes": {CLASS_KEEP: 0, CLASS_REST: 0, CLASS_NOISE: 0},
             "groups": 0}
    for lineno, raw in enumerate(raw_lines, 1):
        entry = parse_line(raw)
        if entry is None:
            stats["blank"] += 1
            continue
        stats["input"] += 1
        entry.lineno = lineno
        key = group_key(entry)
        if golden_keys is not None and key in golden_keys:
            stats["golden_dropped"] += 1
            continue
        if cross_keys is not None and key in cross_keys:
            stats["cross_dropped"] += 1
            continue
        if cross_keys is not None:
            cross_keys[key] = True
        cls = rules.classify(entry)
        g = groups.get(key)
        if g is None:
            groups[key] = {"cls": cls, "occ": [entry]}
        else:
            if CLASS_PRECEDENCE[cls] < CLASS_PRECEDENCE[g["cls"]]:
                g["cls"] = cls  # a keep-always instance promotes the whole group
            g["occ"].append(entry)

    out = []
    stats["groups"] = len(groups)
    # noise rolls up by matched pattern, not by key: distinct DT node lines,
    # memmap entries etc. collapse into one family regardless of their text.
    fam_count = {}
    fam_keys = {}
    for g in groups.values():
        if g["cls"] != CLASS_NOISE:
            continue
        for e in g["occ"]:
            pat = rules.noise_family(e)
            fam_count[pat] = fam_count.get(pat, 0) + 1
            fam_keys.setdefault(pat, {})
            k = group_key(e)
            fam_keys[pat][k] = fam_keys[pat].get(k, 0) + 1
    emitted_families = set()
    for g in groups.values():
        occ, n, cls = g["occ"], len(g["occ"]), g["cls"]
        if cls == CLASS_NOISE:
            for e in occ:
                pat = rules.noise_family(e)
                if pat in emitted_families:
                    continue
                emitted_families.add(pat)
                out.append((e.msg, cls))
                if fam_count[pat] > 1:
                    detail = ""
                    top = sorted(fam_keys[pat].items(), key=lambda kv: -kv[1])
                    if len(top) > 1:
                        detail = " (top: {d})".format(
                            d=", ".join("{k} x{c}".format(k=k, c=c) for k, c in top[:3]))
                    out.append(("  ↳ x{n} lines matched '{p}'{d}".format(
                        n=fam_count[pat], p=pat, d=detail), cls))
                    stats["collapsed"] += fam_count[pat] - 1
            stats["classes"][CLASS_NOISE] = len(emitted_families)
            continue
        stats["classes"][cls] += 1
        if n <= DEDUP_OCCURRENCES:
            shown = occ
        else:
            shown = occ[:1] + [occ[n // 2], occ[-1]]
        out.extend((e.msg, cls) for e in shown)
        if n > 1:
            detail = ""
            if n > DEDUP_OCCURRENCES:
                detail = " (log lines {a}, {b}, {c})".format(
                    a=occ[0].lineno, b=occ[n // 2].lineno, c=occ[-1].lineno)
            out.append(("  ↳ x{n} total{detail}".format(n=n, detail=detail), cls))
            stats["collapsed"] += n - len(shown)

    stats["output"] = sum(len(t) + 1 for t, _ in out)
    if budget_bytes is not None and stats["output"] > budget_bytes:
        out, overflow, stats["elided"] = _apply_budget(out, budget_bytes)
        stats["output"] = sum(len(t) + 1 for t, _ in out)
    else:
        overflow = False

    result = Digest("\n".join(t for t, _ in out) + "\n")
    result.stats = stats
    result.overflow = overflow
    return result


def _apply_budget(entries, budget):
    """Fit the digest to a byte budget: keep-always win again, then head+tail."""
    keeps = [e for e in entries if e[1] == CLASS_KEEP]
    rest = [e for e in entries if e[1] != CLASS_KEEP]
    keep_cost = sum(len(t) + 1 for t, _ in keeps)
    if keep_cost > budget:
        marker = ("  [... overflow: keep-always exceed budget by {d} bytes; "
                  "{n} lower-priority lines elided -- read this log raw]"
                  .format(d=keep_cost - budget, n=len(rest)))
        return keeps + [(marker, CLASS_NOISE)], True, len(rest)

    half = max(0, (budget - keep_cost - BUDGET_MARKER_RESERVE) // 2)
    head, used, cut = [], 0, len(rest)
    for i, e in enumerate(rest):
        cost = len(e[0]) + 1
        if used + cost > half:
            cut = i
            break
        head.append(e)
        used += cost
    tail, used2 = [], 0
    for e in reversed(rest[cut:]):
        cost = len(e[0]) + 1
        if used2 + cost > half:
            break
        tail.append(e)
        used2 += cost
    tail.reverse()
    elided = len(rest) - len(head) - len(tail)
    marker = "  [...skipped {n} lines to fit {b} byte budget]".format(
        n=elided, b=budget)
    return keeps + head + [(marker, CLASS_NOISE)] + tail, False, elided


def suggest(raw_lines, rules=None, min_count=3):
    """Report repeated patterns not covered by the noise rules.

    This is the answer to "new symbols came up": unknown repeats are already
    bounded in the digest, and this ranks them so frequent ones can be promoted
    into a --rules file.
    """
    rules = rules or Rules.default()
    counts, sample = {}, {}
    for raw in raw_lines:
        e = parse_line(raw)
        if e is None or rules.classify(e) != CLASS_REST:
            continue
        k = group_key(e)
        counts[k] = counts.get(k, 0) + 1
        sample.setdefault(k, e.msg)
    return ["x{count}  {key}\n      first: {sample}".format(
        count=counts[k], key=k, sample=sample[k])
        for k in sorted(counts, key=lambda k: (-counts[k], k))
        if counts[k] >= min_count]


def golden_keys(raw_lines):
    """Group keys of a known-good log, for --golden diffing."""
    return {group_key(e) for e in (parse_line(l) for l in raw_lines) if e is not None}


def render_header(stats, budget, source, rules_desc):
    return "\n".join([
        "# logdigest -- {src}".format(src=source),
        "# digest stats: in={inp} out={out} collapsed={col} golden_dropped={gold} "
        "cross_dropped={cross} elided={eli} overflow={ovf}".format(
            inp=stats["input"], out=stats["output"], col=stats["collapsed"],
            gold=stats["golden_dropped"], cross=stats.get("cross_dropped", 0),
            eli=stats.get("elided", 0), ovf=stats.get("overflow", False)),
        "# groups: keep-always={keep} rest={rest} noise={noise} "
        "(keep-always lines are never dropped)".format(**stats["classes"]),
        "# budget: {b} bytes; rules: {r}".format(b=budget, r=rules_desc),
        "",
    ])


def _readlines(path):
    with open(path, errors="replace") as fh:
        return fh.readlines()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="+",
                    help="raw logs (logdump CSV, dmesg, logcat or serial)")
    ap.add_argument("-o", "--out", help="combined output file "
                    "(default: <input>.digest.log per input)")
    ap.add_argument("--golden", help="known-good log; lines already in it are "
                    "dropped and counted (logreduce-style diff)")
    ap.add_argument("--rules", help="file of 'noise: regex' / 'keep: regex' lines")
    ap.add_argument("--budget", type=int, default=DEFAULT_BUDGET,
                    help="digest size budget in KB (default: %(default)s)")
    ap.add_argument("--suggest", action="store_true",
                    help="instead of digesting, list repeated patterns not "
                    "covered by the rules (promotion candidates)")
    ap.add_argument("--cross-dedup", action="store_true",
                    help="dedupe across the input logs (for several boots of "
                    "the same kernel: budget for one boot plus deltas)")
    args = ap.parse_args(argv)

    rules = Rules.default()
    if args.rules:
        base, extra = Rules.default(), Rules.load(args.rules)
        rules = base.merged(extra)
    budget = args.budget * 1024
    try:
        gk = golden_keys(_readlines(args.golden)) if args.golden else None
    except OSError as e:
        print("logdigest: {p}: {e}".format(p=args.golden, e=e), file=sys.stderr)
        return 1
    overflow, combined, notes = False, [], []
    cross = {} if args.cross_dedup else None

    for path in args.logs:
        try:
            raw = _readlines(path)
        except OSError as e:
            print("logdigest: {p}: {e}".format(p=path, e=e), file=sys.stderr)
            return 1
        out_path = args.out or (path + (".suggestions.txt" if args.suggest
                                        else ".digest.log"))
        if args.suggest:
            body = "\n".join(suggest(raw, rules=rules))
            block = ("# promotion candidates for {p} (not covered by rules)\n"
                     .format(p=path)) + body + "\n"
            notes.append("logdigest: {n} promotion candidates for {p}".format(
                n=len(body.splitlines()), p=path))
        else:
            d = digest(raw, rules=rules, golden_keys=gk, budget_bytes=budget,
                       cross_keys=cross)
            overflow = overflow or d.overflow
            block = render_header(d.stats, budget, path,
                                  args.rules or "built-in defaults") + d
            notes.append("logdigest: digest stats: {p} in={i} out={o} collapsed={c} "
                         "golden_dropped={g} cross_dropped={x} elided={e} overflow={ovf}".format(
                             p=path, i=d.stats["input"], o=d.stats["output"],
                             c=d.stats["collapsed"], g=d.stats["golden_dropped"],
                             x=d.stats.get("cross_dropped", 0),
                             e=d.stats.get("elided", 0), ovf=d.overflow))
        if args.out:
            combined.append(block)
        else:
            with open(out_path, "w") as fh:
                fh.write(block)

    if args.out and combined:
        with open(args.out, "w") as fh:
            fh.write("\n".join(combined))

    for line in notes:
        print(line)
    return 2 if overflow else 0


if __name__ == "__main__":
    sys.exit(main())
