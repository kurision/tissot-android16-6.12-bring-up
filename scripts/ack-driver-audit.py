#!/usr/bin/env python3
"""Report which device-tree nodes the kernel tree can actually drive.

A node binds if *any* string in its `compatible` list is matched by an of_match
table in the tree, because DT binding uses fallback lists: "qcom,msm8953-sdhci",
"qcom,sdhci-msm-v4" is bound by the second entry. Scoring individual strings
separately understates coverage badly.

For each bound node the driver file is resolved to the CONFIG symbol its own
directory Makefile assigns, so the output says not just "bound" but "bound and
compiled in".

Usage:
  scripts/ack-driver-audit.py --dtb <path.dtb> --tree <kernel-tree> [--config <.config>]
                              [--only-unbound] [--only-disabled]

Exit status is 0 unless the dtb cannot be read.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path
OF_MATCH_RE = re.compile(r'\.compatible\s*=\s*([^;]+);', re.DOTALL)
QUOTED_RE = re.compile(r'"([^"]+)"')
SCAN_DIRS = ("drivers", "sound", "net", "fs", "arch/arm64")
SKIP_SOURCES = {("drivers", "of", "platform.c")}

# Nodes the kernel core consumes directly rather than binding through of_match:
# CPUs and caches, memory and reservations, OPP tables, timers, firmware.
NOT_A_DEVICE_PREFIX = ("/cpus", "/reserved-memory", "/memory", "/timer", "/chosen",
                       "/firmware", "/aliases", "/soc@0/interrupt-controller")
NOT_A_DEVICE_COMPAT = ("operating-points-v2", "shared-dma-pool", "fixed-clock",
                       "arm,armv8-timer", "armv8-timer", "cache")


def nodes(dtb: Path) -> list[tuple[str, list[str], str]]:
    """(path, compatible list, status) for every node that declares a compatible."""
    listing = subprocess.run(["fdtget", "-l", str(dtb), "/"],
                             check=True, capture_output=True, text=True).stdout.split()
    found: list[tuple[str, list[str], str]] = []

    def walk(path: str) -> None:
        for name in subprocess.run(["fdtget", "-l", str(dtb), path],
                                   check=True, capture_output=True,
                                   text=True).stdout.split():
            child = f"{path.rstrip('/')}/{name}"
            try:
                raw = subprocess.run(["fdtget", "-t", "s", str(dtb), child, "compatible"],
                                     check=True, capture_output=True, text=True).stdout
            except subprocess.CalledProcessError:
                raw = ""
            if raw.strip():
                compat = [c for c in raw.split() if c]
                status = "okay"
                try:
                    status = subprocess.run(["fdtget", "-t", "s", str(dtb), child, "status"],
                                            check=True, capture_output=True,
                                            text=True).stdout.strip() or "okay"
                except subprocess.CalledProcessError:
                    pass
                found.append((child, compat, status))
            walk(child)

    walk("/")
    _ = listing
    return found


def of_matches(tree: Path) -> dict[str, str]:
    """compatible -> driver source file, for every of_match in the tree."""
    mapping: dict[str, str] = {}
    for scan in SCAN_DIRS:
        base = tree / scan
        if not base.is_dir():
            continue
        for src in base.rglob("*.c"):
            if tuple(src.relative_to(tree).parts) in SKIP_SOURCES:
                continue
            try:
                text = src.read_text(errors="ignore")
            except OSError:
                continue
            for rhs in OF_MATCH_RE.findall(text):
                # The right-hand side may be a plain string or a macro list such as
                # OF_MATCH_COMPATIBLE("a"), "b", so take every quoted string in it.
                for value in QUOTED_RE.findall(rhs):
                    mapping.setdefault(value, str(src.relative_to(tree)))
    return mapping


def symbol_table(tree: Path) -> dict[tuple[str, str], str]:
    """(driver directory, object name) -> required CONFIG symbols.

    Handles both the classic `obj-$(CONFIG_X) += file.o` form and the
    composite module style used by drm/msm and the vendored kgsl driver:
    `foo-y += bar.o` lists consumed by `obj-$(CONFIG_X) += foo.o`, plus
    variable expansion (`msm-y += $(adreno-y)`, with or without the -y).
    A member contributed under a conditional list (`foo-$(CONFIG_COND) +=
    bar.o`) requires both symbols; an object listed under several
    alternative obj-$(CONFIG) lines (e.g. per-SoC selects) requires any one
    of them. Alternatives join with `|`, conjunctions with `,`; main() gates
    with any(all()) over them.
    """
    table: dict[tuple[str, str], list[set[str]]] = {}

    def expand(value: str, lists: dict[str, list[str]], depth: int = 0) -> list[str]:
        def repl(m: re.Match) -> str:
            name = m.group(1)
            hit = lists.get(name)
            if hit is None:
                stem = re.sub(r"-(y|m|objs)$", "", name)
                hit = lists.get(stem) if stem != name else None
            return " ".join(hit or []) if depth < 8 else ""

        return re.sub(r"\$\(([A-Za-z0-9_\-\.]+)\)", repl, value).split()

    for makefile in tree.rglob("Makefile"):
        if "out" in makefile.parts or ".git" in makefile.parts:
            continue
        try:
            text = makefile.read_text(errors="ignore")
        except OSError:
            continue
        directory = str(makefile.parent.relative_to(tree))

        # join make continuation lines
        joined, acc = [], ""
        for line in text.splitlines():
            if line.rstrip().endswith("\\"):
                acc += line.rstrip()[:-1] + " "
            else:
                joined.append(acc + line)
                acc = ""
        if acc:
            joined.append(acc)

        obj_defs: list[tuple[str, list[str]]] = []   # (CONFIG symbol, tokens)
        lists: dict[str, list[str]] = {}             # name -> members (names)
        conds: dict[str, set[str]] = {}              # member -> extra CONFIGs

        for line in joined:
            line = line.split("#", 1)[0]
            m = re.match(r"\s*obj-\$\(CONFIG_([A-Za-z0-9_]+)\)\s*[:+]?=\s*(.+)", line)
            if m:
                obj_defs.append((m.group(1), expand(m.group(2), lists)))
                continue
            m = re.match(r"\s*([A-Za-z0-9_\-\.]+)-(?:\$\(CONFIG_([A-Za-z0-9_]+)\))?y?\s*\+?[:?]?=\s*(.+)", line)
            if m:
                name, cond, rhs = m.group(1), m.group(2), expand(m.group(3), lists)
                lists.setdefault(name, [])
                for tok in rhs:
                    lists[name].append(tok)
                    if cond:
                        conds.setdefault(tok, set()).add(f"CONFIG_{cond}")
                continue
            # drag non -y/-m assignments' variable refs into view (foo := $(a) $(b))
            m = re.match(r"\s*([A-Za-z0-9_\-\.]+)\s*:?\+?=\s*(.+)", line)
            if m and "$(" in m.group(2):
                rhs = expand(m.group(2), lists)
                if rhs:
                    lists.setdefault(m.group(1), []).extend(rhs)

        for symbol, tokens in obj_defs:
            for tok in tokens:
                base = tok[:-2] if tok.endswith(".o") else tok
                members = lists.get(base, [tok])
                for member in members:
                    if "/" in member:
                        mdir, _, mobj = member.rpartition("/")
                        key = (directory + "/" + mdir, mobj)
                    else:
                        key = (directory, member)
                    required = {f"CONFIG_{symbol}"} | conds.get(member, set())
                    slot = table.setdefault(key, [])
                    if required not in slot:
                        slot.append(required)

    out: dict[tuple[str, str], str] = {}
    for key, alternatives in table.items():
        shown = ",".join(sorted(alternatives[0]))
        if len(alternatives) > 1:
            shown += "|" + "|".join(",".join(sorted(a)) for a in alternatives[1:])
        out[key] = shown
    return out


def enabled_symbols(config: Path | None) -> set[str]:
    if not config or not config.is_file():
        return set()
    enabled = set()
    for line in config.read_text(errors="ignore").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            if value.strip() in ("y", "m"):
                enabled.add(key)
    return enabled


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dtb", required=True, type=Path)
    parser.add_argument("--tree", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--only-unbound", action="store_true",
                        help="print only nodes with no matching driver")
    parser.add_argument("--only-disabled", action="store_true",
                        help="consider only nodes with status = \"okay\"")
    args = parser.parse_args()

    if not args.dtb.is_file():
        print(f"no such dtb: {args.dtb}", file=sys.stderr)
        return 1

    matches = of_matches(args.tree)
    symbols = symbol_table(args.tree)
    enabled = enabled_symbols(args.config)

    total = driven = built = unbound = core = 0
    for path, compat, status in nodes(args.dtb):
        if args.only_disabled and status != "okay":
            continue
        if path.startswith(NOT_A_DEVICE_PREFIX) or any(c in NOT_A_DEVICE_COMPAT for c in compat):
            core += 1
            continue
        total += 1
        hit = next((c for c in compat if c in matches), None)
        if not hit:
            unbound += 1
            if args.only_unbound:
                print(f"NOBIND   {status:<6} {path:<44} {' '.join(compat)}")
            continue
        driven += 1
        src = matches[hit]
        obj = Path(src).name[:-2] + ".o"
        symbol = symbols.get((str(Path(src).parent), obj), "?")
        is_built = symbol != "?" and any(
            all(s in enabled for s in alt.split(",")) for alt in symbol.split("|"))
        built += int(is_built)
        if not args.only_unbound:
            state = "BUILT  " if is_built else "NOTBLD "
            print(f"{state} {status:<6} {path:<44} {symbol:<34} {src}")

    if not args.only_unbound:
        print(f"\n{total} device nodes: {driven} have a driver ({built} compiled in), "
              f"{unbound} have none, {core} are core-reserved nodes (cpus, opp tables, "
              f"memory, timer)")
        if unbound:
            print(f"Use --only-unbound to list the {unbound} nodes that need a ported "
                  f"or newly written driver.")
    return 0


if __name__ == "__main__":
    sys.exit(main())