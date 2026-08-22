#!/usr/bin/env python3
"""Header-level dependency report from ninja's recorded .ninja_deps.

Reads `ninja -t deps` output (the depfiles the compiler itself emitted during
the build) and answers: which scopehal / scopeprotocols / ngscopeclient headers
does each of our objects actually pull in.
"""
import re, sys, os, collections, subprocess

BUILD = sys.argv[1]
OUT = sys.argv[2]
ROOT = os.path.dirname(os.path.abspath(BUILD))

txt = subprocess.run(["ninja", "-t", "deps"], cwd=BUILD, capture_output=True, text=True).stdout

deps = {}
cur = None
for line in txt.splitlines():
    if not line:
        cur = None
    elif line.startswith("    "):
        if cur:
            deps[cur].add(line.strip())
    else:
        m = re.match(r"^(\S+): #deps (\d+)", line)
        if m and "(VALID)" in line:
            cur = m.group(1)
            deps[cur] = set()
        else:
            cur = None


# main.cpp.o does not build against the current scopehal submodule, so ninja never
# recorded deps for it. Fall back to the .o.d depfile gcc still emits for anything
# missing from .ninja_deps, so the closure stays complete.
for dirpath, _, files in os.walk(os.path.join(BUILD, "src")):
    for fn in files:
        if not fn.endswith(".o.d"):
            continue
        obj = os.path.relpath(os.path.join(dirpath, fn[:-2]), BUILD)
        if obj in deps:
            continue
        body = open(os.path.join(dirpath, fn)).read().replace("\\\n", " ")
        body = body.split(":", 1)[1] if ":" in body else ""
        deps[obj] = set(body.split())


def norm(p):
    p = os.path.normpath(os.path.join(BUILD, p))
    return os.path.relpath(p, ROOT) if p.startswith(ROOT) else p


OUR_TARGETS = ("imcufosphor-core.dir", "imcufosphor-render.dir",
               "sigmf-spectrum.dir", "ngscopeclient-compat.dir")


def owner(obj):
    m = re.search(r"CMakeFiles/([^/]+)\.dir/", obj)
    return m.group(1) if m else "?"


def bucket(h):
    if h.startswith("lib/scopehal-apps/src/ngscopeclient/"):
        return "ngscopeclient"
    if h.startswith("lib/scopehal-apps/src/imgui"):
        return "imgui"
    if h.startswith("lib/scopehal/scopeprotocols/"):
        return "scopeprotocols"
    if h.startswith("lib/scopehal/scopehal/") or h.startswith("lib/scopehal/log/") \
            or h.startswith("lib/scopehal/xptools/"):
        return "scopehal"
    if h.startswith("lib/scopehal/"):
        return "scopehal"
    return None


# header -> set of "target/source.cpp" that include it
users = collections.defaultdict(set)
# also record which of our own TUs are in the closure
tus = []
for obj, hs in deps.items():
    if not any(t in obj for t in OUR_TARGETS):
        continue
    if obj.endswith("cmake_pch.hxx.gch"):
        continue
    tu = os.path.basename(obj)[:-2]                       # strip .o
    tag = f"{owner(obj)}/{tu}"
    tus.append(tag)
    for h in hs:
        n = norm(h)
        b = bucket(n)
        if b:
            users[n].add(tag)

rep = []
rep.append("Headers from scopehal / ngscopeclient reached by the sigmf-spectrum closure")
rep.append("Source: ninja -t deps (compiler-emitted depfiles), build dir " + BUILD)
rep.append(f"Translation units examined: {len(tus)}")
rep.append("")

for b in ("ngscopeclient", "scopehal", "scopeprotocols", "imgui"):
    hs = sorted(h for h in users if bucket(h) == b)
    rep.append("=" * 78)
    rep.append(f"{b}: {len(hs)} headers")
    rep.append("=" * 78)
    for h in hs:
        us = sorted(users[h])
        ours = [u for u in us if not u.startswith("ngscopeclient-compat/")]
        rep.append(f"  {h}")
        rep.append(f"      used by ({len(us)}): " + ", ".join(u.split("/", 1)[1] for u in
                                                              (ours if ours else us)[:10])
                   + (" ..." if len(ours if ours else us) > 10 else ""))
    rep.append("")

open(OUT, "w").write("\n".join(rep) + "\n")
print("\n".join(rep[:4]))
for b in ("ngscopeclient", "scopehal", "scopeprotocols", "imgui"):
    print(f"  {b}: {sum(1 for h in users if bucket(h)==b)} headers")

# ---- graphviz: our files -> the specific scopehal/ngscopeclient headers they use
edges = set()
for h, us in users.items():
    if bucket(h) in ("imgui",):
        continue
    for u in us:
        tgt, src = u.split("/", 1)
        if tgt == "ngscopeclient-compat":
            continue                      # lifted upstream code, not ours
        edges.add((src, h))

COL = {"ngscopeclient": ("#5a2a6b", "#f0dcfa"),
       "scopehal": ("#255c3a", "#dcf2e4"),
       "scopeprotocols": ("#6b4a12", "#faedcf")}
dotp = OUT.replace(".txt", ".dot")
with open(dotp, "w") as f:
    f.write('digraph "sigmf-spectrum -> upstream headers" {\n  rankdir=LR;\n  ranksep=3;\n')
    f.write('  node [shape=box, style="filled,rounded", fontname="Helvetica", fontsize=9];\n')
    f.write('  edge [color="#aaaaaa", arrowsize=0.5];\n')
    srcs = sorted({a for a, _ in edges})
    f.write('  subgraph cluster_ours {\n    label="sigmf-spectrum sources"; color="#1f3b73";\n')
    for s in srcs:
        f.write(f'    "{s}" [color="#1f3b73", fillcolor="#dce6fa"];\n')
    f.write("  }\n")
    for b, (fg, bg) in COL.items():
        hs = sorted({h for _, h in edges if bucket(h) == b})
        if not hs:
            continue
        f.write(f'  subgraph cluster_{b} {{\n    label="{b}"; color="{fg}"; fontname="Helvetica-Bold";\n')
        for h in hs:
            f.write(f'    "{h}" [label="{os.path.basename(h)}", color="{fg}", fillcolor="{bg}"];\n')
        f.write("  }\n")
    for a, b_ in sorted(edges):
        f.write(f'  "{a}" -> "{b_}";\n')
    f.write("}\n")
print(f"wrote {dotp}: {len({a for a,_ in edges})} sources, {len({b for _,b in edges})} headers, {len(edges)} edges")
