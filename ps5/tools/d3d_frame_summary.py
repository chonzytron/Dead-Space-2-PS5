#!/usr/bin/env python3
"""Dead Space 2 for PS5: a recorded frame (logs/d3d_frame_<n>.txt, ps5_d3d_trace = 1)
as a list of passes.

    python3 ps5/tools/d3d_frame_summary.py logs/d3d_frame_001.txt

A pass is the calls between two changes of what is drawn into: the colour
targets (0-3) and the depth target as they are when its first draw is made.
For each: the call numbers, the targets (the surface's address in guest memory,
its size word and its EDRAM word, as recorded), its draws, clears, resolves and
the shaders and textures it set. Surfaces are named S1, S2, ... in the order
they appear, so the same one is easy to follow through the frame.
"""
import re
import sys
from collections import Counter

LINE = re.compile(r"^\s*(\d+) (\w+)((?: [0-9A-F]{8})*)(?: f1=\S+)?((?: \[[^\]]*\])*)\s*$")
DRAWS = {"DrawVertices", "DrawIndexedVertices", "DrawVerticesUP"}


def parse(path):
    calls = []
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            regs = [int(x, 16) for x in m.group(3).split()]
            blocks = [[int(w, 16) for w in b.split()] for b in re.findall(r"\[([^\]]*)\]", m.group(4))]
            calls.append((int(m.group(1)), m.group(2), regs, blocks))
    return calls


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    calls = parse(sys.argv[1])
    names = {}  # (address, size word, EDRAM word) -> (name, description)

    def surface(address, block):
        # (the game rewrites a surface structure in place, e.g. as its 4x MSAA
        # view: a surface is its address and what it holds)
        if not address:
            return "-"
        size, edram = (block[6], block[10]) if block and len(block) >= 11 else (0, 0)
        key = (address, size, edram)
        if key not in names:
            names[key] = (f"S{len(names) + 1}", f"{address:08X}  size {size:08X} edram {edram:08X}")
        return names[key][0]

    color = ["-"] * 4
    depth = "-"
    passes = []
    current = None

    def targets():
        return tuple(color) + (depth,)

    for number, fn, regs, blocks in calls:
        if fn == "SetRenderTarget" and len(regs) >= 3:
            index = regs[1]
            if index < 4:
                color[index] = surface(regs[2], blocks[0] if blocks else None)
            continue
        if fn == "SetDepthStencilSurface" and len(regs) >= 2:
            depth = surface(regs[1], blocks[0] if blocks else None)
            continue
        if fn in DRAWS or fn in ("Clear", "ClearF", "Resolve", "ResolveEx"):
            if current is None or current["targets"] != targets():
                current = {"targets": targets(), "first": number, "last": number,
                           "draws": 0, "clears": 0, "resolves": [], "vs": set(), "ps": set(), "tex": 0}
                passes.append(current)
            current["last"] = number
            if fn in DRAWS:
                current["draws"] += 1
            elif fn == "Clear":
                current["clears"] += 1
            elif fn == "Resolve":
                # (flags r4: which target, 0-3 colour, 4 depth; destination texture r6)
                what = "depth" if regs[1] & 4 else f"colour {regs[1] & 3}"
                current["resolves"].append(f"{what} -> {regs[3]:08X}")
            continue
        if current is not None:
            if fn == "SetVertexShader" and len(regs) >= 2:
                current["vs"].add(regs[1])
            elif fn == "SetPixelShader" and len(regs) >= 2:
                current["ps"].add(regs[1])
            elif fn == "SetTexture":
                current["tex"] += 1

    print(f"{sys.argv[1]}: {len(calls)} calls, {len(passes)} passes, "
          f"{sum(p['draws'] for p in passes)} draws")
    print("\nSurfaces:")
    for name, desc in names.values():
        print(f"  {name:4} {desc}")
    print("\nPasses (colour 0-3 / depth):")
    for p in passes:
        rt = " ".join(p["targets"][:4]) + " / " + p["targets"][4]
        line = f"  {p['first']:5}-{p['last']:<5} {rt:28} draws {p['draws']:4}"
        if p["clears"]:
            line += f", clears {p['clears']}"
        if p["vs"] or p["ps"]:
            line += f", shaders {len(p['vs'])} vs {len(p['ps'])} ps"
        if p["resolves"]:
            line += ", resolve " + "; ".join(p["resolves"])
        print(line)
    switches = Counter()
    for a, b in zip(passes, passes[1:]):
        if a["targets"][4] != b["targets"][4]:
            switches[(a["targets"][4], b["targets"][4])] += 1
    if switches:
        print("\nDepth target changes between passes:")
        for (a, b), n in switches.most_common():
            print(f"  {a} -> {b}: {n}")


if __name__ == "__main__":
    main()
