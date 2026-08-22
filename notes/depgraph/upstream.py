#!/usr/bin/env python3
"""Which scopehal / ngscopeclient files does sigmf-spectrum actually use?

Two different questions, answered from two different sources:

  direct     - what our own files name in an #include.  Taken from the gcc -H
               include tree (depth-marked), which .ninja_deps cannot give since
               it stores a flat set.
  transitive - everything the preprocessor ends up reading.  Taken from
               `ninja -t deps`, i.e. the depfiles the real build emitted.

The gap between the two is the umbrella-header cost: scopehal.h and
scopeprotocols.h include almost their whole tree.
"""
import json, os, re, shlex, subprocess, sys, collections

ROOT = "/data/dkozel/src/projects/imcufosphor"
BUILD = os.path.join(ROOT, "build-ninja")
OUT = sys.argv[1] if len(sys.argv) > 1 else "."

OUR_TARGETS = ("imcufosphor-core.dir", "imcufosphor-render.dir", "sigmf-spectrum.dir")
LIFTED = "ngscopeclient-compat.dir"

# ---------------------------------------------------------------- direct (-H tree)
db = json.load(open(os.path.join(BUILD, "compile_commands.json")))
tus = [e for e in db
       if any(t in e["command"] for t in OUR_TARGETS + (LIFTED,))
       and not e["file"].endswith("cmake_pch.hxx.cxx")]


def strip(cmd):
    argv, out, i = shlex.split(cmd), [], 0
    while i < len(argv):
        a = argv[i]
        if a in ("-o", "-include", "-MT", "-MF"):
            i += 2
            continue
        if a in ("-c", "-Winvalid-pch", "-MD"):
            i += 1
            continue
        out.append(a)
        i += 1
    return out + ["-H", "-fsyntax-only", "-w"]


LINE = re.compile(r"^(\.+) (.*)$")
direct = collections.defaultdict(set)       # our file -> set of upstream headers
tree = collections.defaultdict(set)         # any file -> its direct includes

for e in tus:
    p = subprocess.run(strip(e["command"]), cwd=e["directory"], capture_output=True, text=True)
    stack = {0: os.path.realpath(e["file"])}
    for line in p.stderr.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        d = len(m.group(1))
        path = os.path.realpath(os.path.join(e["directory"], m.group(2)))
        stack[d] = path
        parent = stack.get(d - 1)
        if parent:
            tree[parent].add(path)

# ------------------------------------------------------------------ classification
def rel(p):
    return os.path.relpath(p, ROOT) if p.startswith(ROOT) else p


def kind(p):
    r = rel(p)
    if r.startswith("src/imcufosphor/"):
        return "ours"
    if r.startswith("src/ngscopeclient-compat/"):
        return "ours"
    if r.startswith("lib/scopehal-apps/src/ngscopeclient/"):
        return "ngscopeclient"
    if r.startswith("lib/scopehal-apps/src/imgui"):
        return "imgui"
    if r.startswith("lib/scopehal/scopeprotocols/"):
        return "scopeprotocols"
    if r.startswith("lib/scopehal/"):
        return "scopehal"
    return None


UP = ("ngscopeclient", "scopehal", "scopeprotocols")

# edge set: (our file | upstream header) -> upstream header, first hop only
edges = set()
for parent, kids in tree.items():
    kp = kind(parent)
    if kp is None:
        continue
    for k in kids:
        kk = kind(k)
        if kk in UP and kp != kk:
            edges.add((rel(parent), rel(k)))          # crossing into upstream
        elif kk in UP and kp in UP:
            edges.add((rel(parent), rel(k)))          # upstream internal

crossings = {(a, b) for a, b in edges if kind(os.path.join(ROOT, a)) == "ours"}

# ------------------------------------------------------- transitive (ninja -t deps)
raw = subprocess.run(["ninja", "-t", "deps"], cwd=BUILD, capture_output=True, text=True).stdout
tdeps = collections.defaultdict(set)
cur = None
for line in raw.splitlines():
    if not line:
        cur = None
    elif line.startswith("    ") and cur:
        tdeps[cur].add(os.path.normpath(os.path.join(BUILD, line.strip())))
    else:
        m = re.match(r"^(\S+): #deps", line)
        cur = m.group(1) if (m and "(VALID)" in line) else None
for dp, _, fs in os.walk(os.path.join(BUILD, "src")):
    for fn in fs:
        if fn.endswith(".o.d"):
            obj = os.path.relpath(os.path.join(dp, fn[:-2]), BUILD)
            if obj not in tdeps:
                body = open(os.path.join(dp, fn)).read().replace("\\\n", " ")
                tdeps[obj] = {os.path.normpath(os.path.join(BUILD, x))
                              for x in body.split(":", 1)[1].split()} if ":" in body else set()

trans = collections.defaultdict(set)
for obj, hs in tdeps.items():
    if not any(t in obj for t in OUR_TARGETS + (LIFTED,)):
        continue
    for h in hs:
        k = kind(h)
        if k in UP:
            trans[k].add(rel(h))

# --------------------------------------------------------------------- text report
rep = []
rep.append("scopehal / ngscopeclient usage by sigmf-spectrum")
rep.append("=" * 78)
rep.append("")
rep.append("DIRECT: named in an #include by one of our own files")
rep.append("-" * 78)
by_src = collections.defaultdict(list)
for a, b in sorted(crossings):
    by_src[a].append(b)
for s in sorted(by_src):
    rep.append(f"  {s}")
    for h in sorted(by_src[s]):
        rep.append(f"      {h}")
rep.append("")
rep.append("TRANSITIVE: total distinct headers the preprocessor reads (ninja -t deps)")
rep.append("-" * 78)
for k in UP:
    d = len({b for _, b in crossings if kind(os.path.join(ROOT, b)) == k})
    rep.append(f"  {k:16s} {d:4d} direct   {len(trans[k]):4d} transitive")
rep.append("")
for k in UP:
    rep.append(f"  --- {k}: all {len(trans[k])} headers reached ---")
    for h in sorted(trans[k]):
        mark = "*" if any(b == h for _, b in crossings) else " "
        rep.append(f"    {mark} {h}")
    rep.append("")
rep.append("  (* = directly included by our code, not just dragged in)")
open(os.path.join(OUT, "upstream-usage.txt"), "w").write("\n".join(rep) + "\n")

# ------------------------------------------------------------------------ graphviz
COL = {"ours": ("#1f3b73", "#dce6fa"),
       "ngscopeclient": ("#5a2a6b", "#f0dcfa"),
       "scopehal": ("#255c3a", "#dcf2e4"),
       "scopeprotocols": ("#6b4a12", "#faedcf")}

# Graph 1: direct crossings only, plus one hop of upstream internal structure
# from the headers we actually name.
named = {b for _, b in crossings}
hop = {(a, b) for a, b in edges if a in named and kind(os.path.join(ROOT, b)) in UP}

for fname, es, title in (
        ("upstream-direct.dot", crossings, "sigmf-spectrum -> upstream headers (direct #include only)"),
        ("upstream-onehop.dot", crossings | hop, "sigmf-spectrum -> upstream headers (+ one hop)")):
    nodes = {}
    for a, b in es:
        nodes[a] = kind(os.path.join(ROOT, a))
        nodes[b] = kind(os.path.join(ROOT, b))
    with open(os.path.join(OUT, fname), "w") as f:
        f.write(f'digraph "{title}" {{\n  rankdir=LR;\n  ranksep=2.2;\n')
        f.write('  node [shape=box, style="filled,rounded", fontname="Helvetica", fontsize=10];\n')
        f.write('  edge [color="#aaaaaa", arrowsize=0.6];\n')
        for k, (fg, bg) in COL.items():
            ms = sorted(n for n, kk in nodes.items() if kk == k)
            if not ms:
                continue
            lbl = "sigmf-spectrum sources" if k == "ours" else k
            f.write(f'  subgraph cluster_{k} {{\n    label="{lbl}"; color="{fg}"; fontname="Helvetica-Bold";\n')
            for n in ms:
                f.write(f'    "{n}" [label="{os.path.basename(n)}", color="{fg}", fillcolor="{bg}"];\n')
            f.write("  }\n")
        for a, b in sorted(es):
            f.write(f'  "{a}" -> "{b}";\n')
        f.write("}\n")
    print(f"{fname}: {len(nodes)} nodes {len(es)} edges")

for k in UP:
    d = len({b for _, b in crossings if kind(os.path.join(ROOT, b)) == k})
    print(f"  {k:16s} {d:4d} direct   {len(trans[k]):4d} transitive")
