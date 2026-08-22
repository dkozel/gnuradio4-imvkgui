# Local patches to submodules

Decisions D3 and D4 in `DESIGN.md` say the submodules under `lib/` are consumed
unmodified, so that upstream updates are a submodule bump with no merge burden. Anything
in this directory is a **deliberate, tracked exception** to that rule.

Each patch here must:

- be committed on a branch inside its submodule, named `imcufosphor/<topic>`, so it is a
  real commit with a real message rather than a floating working-tree edit;
- be exported here with `git format-patch`, so it survives a submodule bump, a fresh
  clone, or someone checking out the pinned upstream commit;
- carry a commit message written to be read by the upstream maintainer, not by us —
  these are intended to become upstream pull requests;
- be listed below with its status.

## Current patches

### `scopehal/0001-AcceleratorBuffer-work-around-NVIDIA-Xid-32-on-trans.patch`

Branch `imcufosphor/xid32-workaround` in `lib/scopehal`, on top of `0c6b2f41` (v0.2.2).

Works around an NVIDIA driver defect that makes **every** scopehal GPU transfer fault the
device with `NVRM: Xid 32` / `VK_ERROR_DEVICE_LOST`. Without it, no GPU work is possible on
this workstation at all. See `DESIGN.md` §3 (R7) for the full analysis and
`tools/vkmintest/xid32_repro.cpp` for the 166-line standalone reproduction.

**Status:** local only. Not yet submitted upstream — the intent is to raise it with the
scopehal maintainers once the phase 1 results are in, at which point they may prefer the
`vkCmdSetEvent2` route or identify a different underlying cause.

**This procedure has already failed once.** The v0.2.1 → v0.2.2 bump left the branch behind
on `24dd95fb` and nothing noticed: `master` was checked out, the patch was not in the tree, and
`--verify` faulted with `VK_ERROR_DEVICE_LOST` until it was re-applied here. A re-apply step that
depends on someone remembering is not a mechanism. Treat this as the argument for folding the
change into a vendored copy rather than carrying it as a patch.

**On submodule bump:** re-apply this branch onto the new upstream commit and re-export.
Verify with `./build/tools/wfbench/wfbench --file FILE.sigmf-meta --verify` on the default device — if it reports
`ComplexFFTFilter verification PASSED` without `SCOPEHAL_VULKAN_DEVICE_OVERRIDE`, the patch
is live. If the fault returns, the patch was lost.

## Re-applying from scratch

```
git -C lib/scopehal checkout -b imcufosphor/xid32-workaround
git -C lib/scopehal am ../../patches/scopehal/*.patch
```
