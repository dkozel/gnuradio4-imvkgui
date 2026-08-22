# sigmf-spectrum dependency graphs

Generated from a Ninja build, not from grepping `#include` lines.

## Regenerating

```sh
cmake -S . -B build-ninja -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16 \
	-DBUILD_GR4_BLOCKS=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build-ninja sigmf-spectrum

# build graph (works straight after configure, no compile needed)
ninja -C build-ninja -t graph sigmf-spectrum > notes/depgraph/ninja-graph-raw.dot
python3 notes/depgraph/ninjagraph.py notes/depgraph/ninja-graph-raw.dot notes/depgraph/ninja-build-graph.dot
dot -Tsvg notes/depgraph/ninja-build-graph.dot -o notes/depgraph/ninja-build-graph.svg

# upstream header usage (needs a completed build; reads ninja -t deps)
python3 notes/depgraph/upstream.py notes/depgraph
dot -Tsvg notes/depgraph/upstream-direct.dot -o notes/depgraph/upstream-direct.svg
```

`BUILD_GR4_BLOCKS=OFF` because `pkg-config gnuradio4` is not on this machine's
search path; it does not affect the sigmf-spectrum closure.

## What each file is

| file | what it shows | source of truth |
| --- | --- | --- |
| `ninja-build-graph.svg` | link structure: sources -> static libs -> `sigmf-spectrum`, object counts collapsed per target | `ninja -t graph` |
| `ninja-graph-raw.dot` | the unfiltered ninja graph (1940 nodes) | `ninja -t graph` |
| `upstream-direct.svg` | which scopehal / scopeprotocols / ngscopeclient headers our files name in an `#include` | `gcc -H` include tree |
| `upstream-onehop.svg` | same, plus one level of upstream-internal includes | `gcc -H` include tree |
| `upstream-usage.txt` | direct vs transitive header lists, with `*` marking direct | both |
| `sigmf-spectrum-deps-ours.svg` | project-internal include graph (`src/imcufosphor` only) | `gcc -H` include tree |
| `sigmf-spectrum-deps.svg` | same, with the upstream boundary collapsed to one node per library | `gcc -H` include tree |

`ninja -t deps` gives a flat set per object, so it can say *what was read* but not
*who included it*. The `-H` tree gives parent->child edges. The scripts use each
for the question it can answer.

## Known gap

`src/imcufosphor/main.cpp` does not compile against the currently checked out
scopehal (v0.2.2): `main.cpp:70` calls `QueueManager::GetRenderQueue`, which does
not exist -- `QueueManager.h` offers `GetQueueFromPool(QueuePoolID, name)`.
626 of 627 build steps succeed. `upstream.py` falls back to the `.o.d` depfile gcc
still emits for that TU, so the graphs are complete.
