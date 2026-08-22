#!/usr/bin/env python3
"""Collapse `ninja -t graph sigmf-spectrum` into a readable build graph.

ninja emits one node per file and one ellipse per rule invocation, with hex
pointers for ids.  This resolves rule nodes into direct file->file edges and
then groups the hundreds of object files by the target they belong to, so the
result shows the link structure rather than 627 individual compiles.
"""
import re, sys, os, collections

SRC = sys.argv[1]
OUT = sys.argv[2]

node = {}          # id -> label
shape = {}         # id -> shape
raw = []           # (src_id, dst_id)

N = re.compile(r'^"(0x[0-9a-f]+)" \[label="([^"]*)"(?:, shape=(\w+))?\]')
E = re.compile(r'^"(0x[0-9a-f]+)" -> "(0x[0-9a-f]+)"')

for line in open(SRC):
    line = line.strip()
    m = N.match(line)
    if m:
        node[m.group(1)] = m.group(2)
        shape[m.group(1)] = m.group(3) or "box"
        continue
    m = E.match(line)
    if m:
        raw.append((m.group(1), m.group(2)))

# Resolve rule (ellipse) nodes: every input of a rule feeds every output of it.
rule_in = collections.defaultdict(list)
rule_out = collections.defaultdict(list)
file_edges = set()
for a, b in raw:
    if shape.get(b) == "ellipse":
        rule_in[b].append(a)
    elif shape.get(a) == "ellipse":
        rule_out[a].append(b)
    else:
        file_edges.add((a, b))
for r in set(rule_in) | set(rule_out):
    for a in rule_in[r]:
        for b in rule_out[r]:
            file_edges.add((a, b))


def group(label):
    """Map a ninja path to the bucket it should appear as in the graph."""
    l = label
    if l.endswith(".o"):
        m = re.search(r"CMakeFiles/([^/]+)\.dir/", l)
        tgt = m.group(1) if m else "?"
        return f"objs:{tgt}", "obj"
    if l.endswith((".so", ".a")) or ".so." in l:
        return f"lib:{os.path.basename(l)}", "lib"
    if l.endswith((".cpp", ".c", ".cxx", ".cc")):
        if "/src/imcufosphor/" in l or l.startswith("../src/imcufosphor"):
            return f"src:{os.path.basename(l)}", "oursrc"
        m = re.search(r"lib/([^/]+)/", l)
        return f"src:{m.group(1) if m else 'other'} sources", "extsrc"
    if l.endswith((".spv", ".glsl")) or "shader" in l.lower():
        return "shaders", "asset"
    if l in ("sigmf-spectrum",) or l.endswith("/sigmf-spectrum"):
        return "sigmf-spectrum", "bin"
    return None, None


agg = set()
kinds = {}
for a, b in file_edges:
    ga, ka = group(node.get(a, ""))
    gb, kb = group(node.get(b, ""))
    if not ga or not gb or ga == gb:
        continue
    kinds[ga] = ka
    kinds[gb] = kb
    agg.add((ga, gb))

STYLE = {
    "bin":    ("#1f3b73", "#dce6fa"),
    "oursrc": ("#255c3a", "#dcf2e4"),
    "obj":    ("#6b4a12", "#faedcf"),
    "lib":    ("#5a2a6b", "#f0dcfa"),
    "extsrc": ("#555555", "#ebebeb"),
    "asset":  ("#777777", "#f2f2f2"),
}

with open(OUT, "w") as f:
    f.write('digraph "sigmf-spectrum build" {\n  rankdir=LR;\n')
    f.write('  node [shape=box, style="filled,rounded", fontname="Helvetica", fontsize=10];\n')
    f.write('  edge [color="#999999", arrowsize=0.6];\n')
    for n, k in sorted(kinds.items()):
        fg, bg = STYLE.get(k, ("#555", "#eee"))
        lbl = n.split(":", 1)[-1]
        f.write(f'  "{n}" [label="{lbl}", color="{fg}", fillcolor="{bg}"];\n')
    for a, b in sorted(agg):
        f.write(f'  "{a}" -> "{b}";\n')
    f.write("}\n")

print(f"{len(node)} ninja nodes, {len(file_edges)} file edges -> {len(kinds)} groups, {len(agg)} edges")
# object counts per target, for the report
counts = collections.Counter()
for i, l in node.items():
    if l.endswith(".o"):
        m = re.search(r"CMakeFiles/([^/]+)\.dir/", l)
        counts[m.group(1) if m else "?"] += 1
for t, c in counts.most_common():
    print(f"  {c:4d} objects  {t}")
