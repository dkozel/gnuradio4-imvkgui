#!/usr/bin/env python3
"""Include-dependency graph for the sigmf-spectrum link closure.

Replays every translation unit from compile_commands.json with `-H -fsyntax-only`,
parses the depth-marked include tree gcc prints on stderr, and emits real
parent->child edges (not a flattened dependency list).
"""
import json, os, re, shlex, subprocess, sys, collections

ROOT = "/data/dkozel/src/projects/imcufosphor"
DB = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-g16/compile_commands.json")
OUT = sys.argv[2] if len(sys.argv) > 2 else "."

# The three targets that link into sigmf-spectrum.
TARGETS = ("imcufosphor-core.dir", "imcufosphor-render.dir", "sigmf-spectrum.dir")

db = json.load(open(DB))
tus = [e for e in db
       if any(t in e["command"] for t in TARGETS)
       and not e["file"].endswith("cmake_pch.hxx.cxx")]

edges = collections.Counter()
tu_of = collections.defaultdict(set)


def strip(cmd):
    argv = shlex.split(cmd)
    out, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a in ("-o", "-include"):          # drop output file and the forced PCH
            i += 2
            continue
        if a in ("-c", "-Winvalid-pch"):
            i += 1
            continue
        out.append(a)
        i += 1
    return out + ["-H", "-fsyntax-only", "-w"]


LINE = re.compile(r"^(\.+) (.*)$")

for e in tus:
    argv = strip(e["command"])
    p = subprocess.run(argv, cwd=e["directory"], capture_output=True, text=True)
    stack = {0: os.path.realpath(e["file"])}
    for line in p.stderr.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        depth = len(m.group(1))
        path = os.path.realpath(os.path.join(e["directory"], m.group(2)))
        stack[depth] = path
        parent = stack.get(depth - 1)
        if parent:
            edges[(parent, path)] += 1
        tu_of[path].add(os.path.basename(e["file"]))
    print(f"  {os.path.basename(e['file']):24s} {len(p.stderr.splitlines()):5d} include lines",
          file=sys.stderr)

# ---------------------------------------------------------------- classification

def zone(p):
    r = os.path.relpath(p, ROOT) if p.startswith(ROOT) else p
    if r.startswith("src/imcufosphor/"):
        base = os.path.basename(r)
        return ("app" if base in ("main.cpp", "MainWindow.cpp", "MainWindow.h") else "ours"), r
    if r.startswith("build") and "/src/imcufosphor/" in r:
        return "ours", r
    if r.startswith("src/ngscopeclient-compat/"):
        return "compat", r
    if r.startswith("lib/scopehal-apps/src/ngscopeclient/"):
        return "ngscopeclient", r
    if r.startswith("lib/scopehal-apps/src/imgui"):
        return "ext", "imgui"
    if r.startswith("lib/scopehal/scopeprotocols/"):
        return "ext", "scopeprotocols"
    if r.startswith("lib/scopehal/VkFFT"):
        return "ext", "VkFFT"
    if r.startswith("lib/scopehal/"):
        return "ext", "scopehal"
    if r.startswith("lib/libsigmf/"):
        return "ext", "libsigmf"
    if r.startswith("lib/"):
        return "ext", r.split("/")[1]
    return "sys", "<system / toolchain>"


def label(p):
    z, n = zone(p)
    return n if z in ("ext", "sys") else os.path.basename(n)


# --------------------------------------------------------------- graph emission
COLORS = {
    "app":           ("#1f3b73", "#dce6fa"),
    "ours":          ("#255c3a", "#dcf2e4"),
    "compat":        ("#6b4a12", "#faedcf"),
    "ngscopeclient": ("#5a2a6b", "#f0dcfa"),
    "ext":           ("#555555", "#ebebeb"),
    "sys":           ("#888888", "#f7f7f7"),
}

def emit(path, keep_zones, name, rankdir="LR"):
    nodes, ge = {}, set()
    for (a, b), _ in edges.items():
        za, na = zone(a)
        zb, nb = zone(b)
        if za not in keep_zones or zb not in keep_zones:
            continue
        if na == nb:
            continue
        nodes[na] = za
        nodes[nb] = zb
        ge.add((na, nb))
    with open(path, "w") as f:
        f.write(f'digraph "{name}" {{\n')
        f.write(f'  rankdir={rankdir};\n  splines=true; overlap=false; concentrate=true;\n')
        f.write('  node [shape=box, style="filled,rounded", fontname="Helvetica", fontsize=10];\n')
        f.write('  edge [color="#999999", arrowsize=0.6];\n')
        for z in COLORS:
            members = sorted(n for n, zz in nodes.items() if zz == z)
            if not members:
                continue
            fg, bg = COLORS[z]
            f.write(f'  subgraph cluster_{z} {{\n    label="{z}"; color="{fg}"; fontname="Helvetica-Bold";\n')
            for n in members:
                f.write(f'    "{n}" [label="{os.path.basename(n)}", color="{fg}", fillcolor="{bg}"];\n')
            f.write("  }\n")
        for a, b in sorted(ge):
            f.write(f'  "{a}" -> "{b}";\n')
        f.write("}\n")
    return len(nodes), len(ge)


os.makedirs(OUT, exist_ok=True)
n1 = emit(os.path.join(OUT, "sigmf-spectrum-deps.dot"),
          {"app", "ours", "compat", "ngscopeclient", "ext"}, "sigmf-spectrum")
n2 = emit(os.path.join(OUT, "sigmf-spectrum-deps-ours.dot"),
          {"app", "ours", "compat"}, "sigmf-spectrum (project files only)")
print(f"full:  {n1[0]} nodes {n1[1]} edges", file=sys.stderr)
print(f"ours:  {n2[0]} nodes {n2[1]} edges", file=sys.stderr)

# ------------------------------------------------------------------ text report
rep = []
ours = {}
for (a, b) in edges:
    za, na = zone(a)
    if za in ("app", "ours", "compat"):
        ours.setdefault(na, set()).add(zone(b)[1])
rep.append("sigmf-spectrum include dependencies (project files -> what they pull in)\n")
for n in sorted(ours):
    deps = sorted(ours[n])
    proj = [d for d in deps if d.startswith("src/") or d.startswith("build")]
    ext = [d for d in deps if d not in proj]
    rep.append(f"{n}")
    if proj:
        rep.append("    project: " + ", ".join(os.path.basename(d) for d in proj))
    if ext:
        rep.append("    extern : " + ", ".join(ext))
open(os.path.join(OUT, "sigmf-spectrum-deps.txt"), "w").write("\n".join(rep) + "\n")
