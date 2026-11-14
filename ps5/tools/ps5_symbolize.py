#!/usr/bin/env python3
"""Turn the addresses in a Dead Space 2 for PS5 log into function names.

usage: python3 ps5/tools/ps5_symbolize.py <out/ps5/ps5-link/build/llvm-pie.elf> <log file or ->

The app's "[ps5 crash]" line and its "ps5 stack:" lines print code addresses
relative to an anchor inside the program plus 0x10000000 (the program is loaded
at a different address on every start). This looks the anchor up in the linked
program (kept by ps5_package.sh) and prints each such line again with
function names and, for the recompiled game code, source lines.

The profiler's "ps5 prof: tid" lines (ps5_profile = true in ds2.toml) are
printed the same way: each place a thread was found at, how often (as a share
of that thread's samples), and its name. A place marked "~" is outside the
program (the C library, the system's libraries, the kernel): the name is the
function in the program that called out. "~0" had no caller in reach.

Needs llvm-nm and llvm-symbolizer (any LLVM; versioned names are found too).
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys

ANCHOR = "rex::ps5diag::NowMs()"
BIAS = 0x10000000


def tool(name: str) -> str:
    for candidate in [name] + [f"{name}-{v}" for v in range(21, 13, -1)]:
        if shutil.which(candidate):
            return candidate
    sys.exit(f"{name} not found")


def anchor_address(elf: str) -> int:
    out = subprocess.run([tool("llvm-nm"), "-C", "--defined-only", elf], capture_output=True, text=True, check=True)
    for line in out.stdout.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[2].strip() == ANCHOR:
            return int(parts[0], 16)
    sys.exit(f"{ANCHOR} is not in {elf}: is it the llvm-pie.elf of the build that wrote the log?")


def relative_addresses(line: str) -> list[int]:
    """The program-relative code addresses in a crash or stack line, in the order printed."""
    if "[ps5 crash]" in line:
        # "... (rip <absolute>, anchor-relative <relative>) | stack: <relative> <relative> ..."
        head, _, stack = line[line.index("[ps5 crash]"):].partition("| stack:")
        found = re.findall(r"anchor-relative ([0-9A-F]+)", head) + re.findall(r"\b[0-9A-F]+\b", stack)
    else:
        # "ps5 stack: tid <n> <name>: rip <relative> | <relative> <relative> ..."
        head, _, stack = line[line.index("ps5 stack:"):].partition("|")
        found = re.findall(r"rip ([0-9A-F]+)", head) + re.findall(r"\b[0-9A-F]+\b", stack)
    values = [int(v, 16) for v in found]
    # (the program is under 256 MB either side of the anchor)
    return [v for v in values if 0 < v < 2 * BIAS]


def symbolize(symbolizer: str, elf: str, addresses: list[int]) -> list[list[str]]:
    """The names for each address: the function, then the ones it was inlined into."""
    if not addresses:
        return []
    out = subprocess.run([symbolizer, "-e", elf, "-C", "-p", *[hex(a) for a in addresses]],
                         capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout.strip():
        sys.exit(f"{symbolizer} failed: {out.stderr.strip() or 'no output'}")
    # One block per address, a blank line between them; an inlined function
    # adds "(inlined by)" lines to its block.
    blocks = out.stdout.strip("\n").split("\n\n")
    return [[name.strip() for name in block.splitlines() if name.strip()] or ["??"] for block in blocks]


def profile_line(line: str, anchor: int, symbolizer: str, elf: str) -> None:
    """A "ps5 prof: tid ... | place*count ..." line: the places, most frequent first, with names."""
    places = re.findall(r"(~?)([0-9A-F]+)\*(\d+)", line.partition("|")[2])
    samples = re.search(r"samples (\d+)", line)
    total = int(samples.group(1)) if samples else sum(int(count) for _, _, count in places)
    known = [(mark, int(value, 16), int(count)) for mark, value, count in places]
    lookups = [v - BIAS + anchor for _, v, _ in known if v - BIAS + anchor > 0 and v]
    names = iter(symbolize(symbolizer, elf, lookups))
    for mark, value, count in known:
        share = 100.0 * count / total if total else 0.0
        if not value:
            print(f"    {share:5.1f}%  ~ outside the program (no caller in reach)")
            continue
        block = next(names) if value - BIAS + anchor > 0 else ["??"]
        print(f"    {share:5.1f}%  {'~ called from ' if mark else ''}{block[0]}")
        for name in block[1:]:
            print(f"            {name}")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    elf, log = sys.argv[1], sys.argv[2]
    anchor = anchor_address(elf)
    text = sys.stdin.read() if log == "-" else open(log, encoding="utf-8", errors="replace").read()
    symbolizer = tool("llvm-symbolizer")
    for line in text.splitlines():
        if "ps5 prof:" in line:
            print(line)
            if "ps5 prof: tid" in line:
                profile_line(line, anchor, symbolizer, elf)
            continue
        if "[ps5 crash]" not in line and "ps5 stack:" not in line:
            continue
        print(line)
        if "ps5 stack: anchor" in line:
            continue
        # (a stack also holds numbers that are no code address: one below the
        # program's start would be a negative address, which stops the symbolizer)
        values = [v for v in relative_addresses(line) if v - BIAS + anchor > 0]
        if not values:
            continue
        for value, names in zip(values, symbolize(symbolizer, elf, [v - BIAS + anchor for v in values])):
            print(f"    {value:X}  {names[0]}")
            for name in names[1:]:
                print(f"        {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
