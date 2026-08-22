# Annotation overlay — implementation plan

Supersedes DESIGN.md §9, which was written before `RowHistory` moved into `PlayerSession`
and before the FFT emitted a batch per block. Three of its claims turn out to be wrong;
see §8.

Verified against the working tree at `d56ccb4` plus uncommitted changes to
`ComplexFFTFilter.cpp` and `SigMFSource.cpp`.

---

## 1. Decisions

| # | Decision | Consequence |
|---|----------|-------------|
| A1 | A display-neutral `AnnotationSet` is built once at open and owned by `PlayerSession`, beside `RowHistory` | Displays include `Annotation.h` only. No libsigmf, no flatbuffers, no `SigMFRecord` in the display layer. |
| A2 | `AnalyzerPane` owns one `AnnotationOverlay` and draws it; the areas expose the plot rect they just drew into | Areas stay waveform renderers. No ruler arithmetic duplicated between pane and area. |
| A3 | Spectrum spans show the current block, fading out afterwards on the density map's own decay constant | Spans do not pop in and out as playback crosses an annotation edge. One knob, shared with the phosphor. |
| A4 | Phase 1 is rendering only — boxes, spans, labels, colour, culling | No hit-testing, no seek, no filter panel. All three are later increments against the same rects. |

## 2. Data flow

```
SigMFSource            (libsigmf lives here and nowhere else)
  m_record : SigMFRecord
  BuildAnnotationSet(m_record, sampleRate) ──┐
                                             │  once, at open
PlayerSession                                v
  m_annotations : AnnotationSet   ◄──────────┘
  m_rowHistory  : RowHistory
      │  const AnnotationSet&, const RowHistory&
      v
AnalyzerPane
  m_overlay : AnnotationOverlay
      │  DrawOnSpectrum(dl, rect, xaxis, now)
      │  DrawOnWaterfall(dl, rect, xaxis, rowHistory)
      v
SpectrumArea / WaterfallArea   — untouched except for GetPlotRect()
```

## 3. Files

| File | Status | Contents |
|------|--------|----------|
| `Annotation.h` | new | `Annotation` POD, `AnnotationSet` with sorted storage and overlap query. No dependency on anything — not sigmf, not ImGui, not scopehal. |
| `Annotation.cpp` | new | `AnnotationSet::Add/Finalize/QueryOverlapping`, label→colour hash. |
| `AnnotationOverlay.{h,cpp}` | new | Geometry and drawing. Depends on `Annotation.h`, `PlotAxis.h`, `RowHistory.h`, ImGui. |
| `SigMFSource.{h,cpp}` | edit | `BuildAnnotationSet()` free function. The one place both type systems are visible. |
| `PlayerSession.{h,cpp}` | edit | Owns the set, builds it in the ctor, exposes the density decay constant. The current block's sample range is already there as `GetCurrentBlock()`. |
| `RowHistory.h` | ~~edit~~ done | `RowMark` carries both the stream and recording coordinate. See §8.2 and `notes/time-domain-plan.md`. |
| `AnalyzerPane.{h,cpp}` | edit | Owns the overlay, calls it after each area's `Render()`. |
| `SpectrumArea.h` / `WaterfallArea.{h,cpp}` | edit | `GetPlotRect()` only. |
| `SpectrumDensity.h` | edit | `GetDecaySeconds()` getter. |
| `MainWindow.cpp` | edit | One `Checkbox("Annotations")` in `RenderSpectrumControls()`. |

## 4. The neutral model

```cpp
struct Annotation
{
	int64_t sampleStart;      //recording coordinates, always valid
	int64_t sampleEnd;        //exclusive; == sampleStart when sample_count is absent
	double  freqLoHz;         //absolute Hz
	double  freqHiHz;
	bool    hasFreq;          //both edges present
	std::string label;        //may be empty
	std::string description;  //kept for the phase-2 tooltip
	std::string comment;
	std::string generator;
	uint32_t colorKey;        //hash of label, resolved to a colour at draw time
};
```

`AnnotationSet` holds a `std::vector<Annotation>` sorted by `sampleStart`, plus a parallel
`std::vector<int64_t> m_maxEndPrefix` — a running maximum of `sampleEnd` over `[0, i]`.

**Why the prefix array.** Sorting by start alone does not make an overlap query a clean
binary search: a long annotation starting far before the window still overlaps it. The
usual answers are an interval tree or a segment tree; the running-max prefix is six lines
and gives the same asymptotics for a static set. Query `[lo, hi)`:

1. `upper_bound` on `sampleStart` for `hi` → the last candidate.
2. Walk backwards from there. Emit anything with `sampleEnd > lo`. Stop as soon as
   `m_maxEndPrefix[i] <= lo`, because nothing at or before `i` can reach the window.

Cost is `O(log n + hits)` in the normal case. On the 9182-annotation omnisig set this is
a handful of comparisons per query, against `get_sigmf_in_range()`'s full copy-and-sort.

Zero-length annotations (`sample_count` absent) get `sampleEnd == sampleStart` and are
treated as overlapping `[lo, hi)` when `sampleStart` is inside it, so they still draw as a
one-pixel line rather than vanishing.

## 5. Conversion, in `SigMFSource.cpp`

```cpp
void BuildAnnotationSet(const SigMFRecord& rec, AnnotationSet& out);
```

- Skip any annotation with no `sample_start` — it is unplaceable, and libsigmf's own
  helper hangs on it (§8.1).
- `sample_count` absent → zero length.
- `freq_lower_edge` / `freq_upper_edge` are independently optional. Treat "both present"
  as the only case with a frequency extent; one edge alone is not a band. `hasFreq ==
  false` becomes a full-width band (§7).
- Swap the edges if they arrive reversed, rather than drawing a negative-width rect.
- Clamp `sampleEnd` to `GetTotalSamples()`.
- `colorKey = FNV-1a(label)`. Empty label hashes to a fixed neutral grey.
- Sort, build the prefix array, log the count and how many were dropped.

Called from the `PlayerSession` constructor, which already holds the source.

## 6. Waterfall geometry

### Row ↔ pixel

The mapping is exact, with no modular arithmetic needed on our side. From
`WaterfallToneMap.glsl:56-63`, output texture row `oy` reads ring row
`(oy + height - outheight + writerow) mod height`, so `oy = outheight-1` is the newest
row. `WaterfallArea::Render()` blits with v flipped (`ImVec2(0,1)`→`ImVec2(1,0)`) at
exactly `plotSize`, one texel per pixel. Therefore:

> **screen y from the top of the plot == age in rows.**

`RowHistory` is already indexed by age. So the overlay never touches `writerow`.

Note `m_waterfall->GetHeight()` is rounded up to a power of two
(`WaterfallArea.cpp:96`) while only `outheight == plotSize.y` rows are visible. Ages
`>= plotSize.y` exist in the ring but are off-screen; the sweep below simply stops at the
plot's bottom edge.

### The sweep

Playback loops, so recording-relative row starts are *piecewise* monotonic — they run
upward, then drop to zero at each wrap. A binary search over them is invalid. Instead,
walk rows top to bottom once per frame (≤ plot height ≈ 600 iterations) and cut the
column into **monotonic runs**, each a contiguous band of screen rows whose sample starts
increase:

```
for each run [yTop, yBottom) covering samples [sLo, sHi):
    for each annotation in set.QueryOverlapping(sLo, sHi):
        yA = RowToY(run, annotation.sampleEnd)     //newer edge, higher on screen
        yB = RowToY(run, annotation.sampleStart)
        draw box clamped to the run
```

Typically one run; two while a wrap is on screen. `RowToY` interpolates within the run by
binary search over that run's slice, which is monotonic by construction. An annotation
spanning a wrap correctly draws as two bands, which is what actually happened.

The run boundaries also give the overlay a cheap `AnnotationSet` query count: one binary
search per run per frame, not one per annotation.

## 7. Spectrum geometry and the fade

**Already built.** `PlayerSession::GetCurrentBlock()` returns a `BlockSpan` holding the
sample range of the block most recently processed, in both coordinate systems. The start is
captured *before* `AcquireData()` in `StepOneBlock()`, since acquisition advances the
cursor, and the end is read back afterwards rather than assumed — a block at the end of the
recording is short, and with looping on the cursor has already wrapped.

Use `recordingStart` / `recordingEnd` for annotation queries. Note the recording range
wraps: a block straddling the end of the file has `recordingEnd < recordingStart`, which the
query has to handle. The stream range beside it is monotonic and is what any elapsed-time
arithmetic should use.

Spans are queried over `[blockStart - decayWindow, blockEnd)` and drawn with

```
ageSec = (blockEnd - annotation.sampleEnd) / sampleRate     //0 while still active
alpha  = baseAlpha * exp(-ageSec / decaySec)
```

`decaySec` comes from `SpectrumDensity::GetDecaySeconds()` (default 0.150 s), so the spans
fade on the same time constant as the phosphor behind them and there is one control rather
than two. `decayWindow = 4 * decaySec * sampleRate` — past that the term is under 2% and
the annotation is dropped rather than drawn invisibly.

This is stateless: alpha is a pure function of the annotation and the current block, so
there is no per-annotation fade state to reset on seek, restart, or resize, and the fade
is independent of frame rate.

## 8. What DESIGN.md §9 got wrong

### 8.1 `get_sigmf_in_range()` cannot be called per frame

§9 proposes it as the query for both panes. Reading
`lib/libsigmf/src/sigmf_helpers.h:94-130`, it:

- returns a **whole new `sigmf::SigMF`** by value, copying globals, captures and every
  matching annotation, with their strings;
- calls `sort_sigmf_vector()` on both `input_md.captures` and `input_md.annotations` on
  every call, i.e. re-sorts an already-sorted 9182-element vector each frame;
- **rebases** `sample_start` to be relative to the segment (lines 106-111), so the result
  is not in the coordinates you asked about;
- takes a non-const reference to the record, so it cannot be called from a display holding
  a `const SigMFRecord&` in the first place; and
- **hangs** on an annotation with no `sample_start`: line 123 `continue`s without
  advancing the iterator (`sigmf_helpers.h:123-125`).

The last one is an upstream bug worth reporting. None of this matters to us once the model
is converted, which is a second, independent reason for the boundary A1 puts in place.

### 8.2 `RowHistory` is in delivered coordinates, not recording coordinates

`PlayerSession.cpp:288` pushes `m_source->GetSamplesDelivered()`, deliberately — the play
cursor wraps on loop and would make the time axis span go negative. But annotation sample
indices are *recording* coordinates. The two only agree on the first pass through the file.

**Done, in `notes/time-domain-plan.md`.** `RowHistory` now stores a `RowMark` per row
carrying both coordinates, `Push()` takes both, and `SigMFSource` has a monotonic
`m_samplesPlayed` that `ResetIngestStats()` does not touch. One push, two views, no modulo
guesswork — and it stays correct if seeking is added, which a modulo would not.

Two refinements to what this section originally proposed:

- **One struct array, not two parallel vectors.** Both fields are written by the same
  `Push()` and read together by the §6 sweep. Two vectors kept in step by hand is a
  desynchronisation waiting to happen for no benefit. (`m_maxEndPrefix` in §4 stays a
  parallel array — it genuinely is scanned independently of the annotations.)
- The accessor is `GetRow(age, RowMark&)`, and `GetSampleSpan()` is now `GetStreamSpan()`
  so the coordinate is named at the call site rather than assumed.

**The related hazard was real and is closed.** `GetSamplesDelivered()` is an *ingest
statistics* counter that `ResetIngestStats()` zeroes, and `PlayerSession::ResetStats()`
calls that. Nothing in the app called `ResetStats()` — only `wfbench.cpp:584` does — so the
waterfall timeline survived by luck. The rule now written down is that an instrumentation
counter must never be a time base.

### 8.3 The overlay does not need to reproduce the shader's wraparound

§9 says vertical placement must reproduce the `(height - outheight) + writerow`
arithmetic. It does not: `RowHistory` is indexed by age, and age is the screen row (§6).
That paragraph describes the mapping the *shader* does, not the one the overlay does.

### 8.4 Minor: the axis centre is float-rounded

`ComplexFFTFilter.cpp:311` builds `m_triggerPhase` from the float `center` scalar stream,
which `SigMFSource.h:130-138` documents as good to only ~128-256 Hz at GHz carriers.
Annotation edges are parsed as doubles. So a box's edges are more accurate than the axis
they are drawn against, and at a 2.4 GHz carrier a narrow annotation can sit up to a
quarter-bin off the feature it describes. Not an annotation bug and not in scope here —
but it is the first feature that makes the discrepancy visible, and it argues for
eventually feeding `GetExactCenterFrequency()` into the axis instead.

## 9. Drawing details

Both panes, common:

- Frequency → pixels through `m_xAxis` only. Annotation Hz × 1e6 → µHz, the axis unit
  (`AnalyzerPane.cpp:23`). Doubles are exact here: 2.4 GHz is 2.4e15 µHz against a 9.0e15
  integer limit, per the note in `PlotAxis.h:31-39`.
- Colour: `colorKey` → hue via golden-ratio conjugate stepping, fixed S and V, so adjacent
  labels get well-separated hues and the same emitter keeps its colour in both panes.
- `AddRectFilled` at ~12% alpha, `AddRect` outline at ~60%, inside a `PushClipRect` on the
  plot rect so boxes running past the edges clip rather than bleeding into the rulers.
- Drop anything under 1 px wide after clamping; draw it as a 1 px line instead of skipping,
  so a narrow emitter is still visible when zoomed out.
- Labels: only when the box is wide enough for the text, and de-duplicated with the
  `neighborThresholdPixels` pattern from `WaveformArea.cpp:1524` so a dense set does not
  turn into a solid bar of text.

Waterfall-specific: `hasFreq == false` → full-width horizontal band, since the annotation
says something about a time range and nothing about frequency.

Spectrum-specific: full plot height between the two frequency pixels; label along the top;
`hasFreq == false` annotations get a thin tint at the top margin rather than a full-width
wash that would swamp the trace.

## 10. Order of work — all done

1. ✅ `Annotation.{h,cpp}` — model, sorted query, prefix array. Standalone; tested with no
   Vulkan, including a brute-force cross-check.
2. ✅ `RowHistory` second coordinate + `SigMFSource::m_samplesPlayed` (§8.2). See
   `notes/time-domain-plan.md`.
3. ✅ `BuildAnnotationSet()` in `SigMFSource.cpp`. The current block range was already there
   as `PlayerSession::GetCurrentBlock()`, captured either side of `AcquireData()`.
4. ✅ `GetPlotRect()` on both areas, returning a `PlotRect` in `PlotAxis.h`.
5. ✅ `AnnotationOverlay::DrawOnSpectrum`.
6. ✅ `AnnotationOverlay::DrawOnWaterfall` — monotonic-run sweep.
7. ✅ `AnalyzerPane` wiring, `MainWindow` checkbox. Off by default.

## 11. What the plan did not anticipate

### 11.1 Per-annotation drawing saturates on a real set

The plan's §9 worries about *labels* crowding and says nothing about *boxes* crowding. On
dect6 that turned out to be the dominant problem. All 2000 of its annotations share one
frequency band, one waterfall row covers about twenty of them, and twenty rectangles at 12%
alpha stacked in one row of pixels is opaque. The first working build rendered both panes as
a solid magenta slab that said nothing about where the signal was. The spectrum was worse:
every match is drawn at full plot height, so they all stacked.

Fix: boxes are collected into a scratch list, merged where they describe the same colour and
frequency extent at adjacent times, and only then drawn (`AnnotationOverlay::FlushBoxes`).
dect6 goes from 18612 matches on screen to **2 boxes**, one per pane. Merging stops at a real
gap in time, so a burst pattern still reads as bursts rather than one long transmission.

Coalescing is strictly a rendering concern: the query still returns every annotation, and the
status line reports set size, matches on screen, and boxes drawn separately.

### 11.2 The matched count legitimately exceeds the set size

18612 matches against a 2000-annotation set is not a bug. dect6 is 10 s long and the waterfall
was showing ~90 s of playback, so the recording had looped about nine times and each
annotation was genuinely on screen nine times over. The monotonic-run sweep is what makes that
correct rather than smeared, and the three-number status readout is what makes it legible.

### 11.3 Where the set lives

§A1 and §3 put `AnnotationSet` in `PlayerSession`. It is in `SigMFSource` instead, beside
`GetClock()`: both are display-neutral views derived from the metadata at load, and the
metadata is there. The property that actually mattered — nothing downstream of `SigMFSource`
sees libsigmf — is unchanged.

### 11.4 Label de-duplication has to be two dimensional

§9 borrows the `neighborThresholdPixels` pattern from `WaveformArea.cpp:1524`, which is
one dimensional because a waveform's labels all sit on one line. On the waterfall, two
annotations at the same frequency and different times are exactly what the display is for, and
an x-only test silently drops one of them.

## 12. Verification

- `Annotation.cpp`: 25 assertions, including zero-length annotations at every window edge, the
  long-annotation case a start-sorted binary search alone gets wrong, and a brute-force
  cross-check of 2000 random annotations against 2000 random windows.
- `BuildAnnotationSet()` over the demo dataset: dect6 2000/2000 with frequency, IQ_800MHz
  568 "P25", lora 522 "LoRa", tone_signal 1 with no frequency extent. No annotation ends past
  the end of its recording, and a whole-recording query returns every annotation in each.
- On screen: dect6 gives one labelled band per pane over 1920.8–1922.3 MHz; tone_signal gives
  the degenerate no-frequency case as a thin top strip on the spectrum and a full-width band on
  the waterfall.
- `wfbench --verify` still passes, so none of this disturbed the pipeline.

Not exercised: the loop-wrap path drawing an annotation as two bands is reasoned through and
falls out of the run sweep, but no capture in the dataset was set up to isolate it visually.
`IQ_800MHz-omnisig.sigmf-meta`, the 9182-annotation crowding case, could not be opened — its
data file basename does not match, a pre-existing issue noted at `DESIGN.md:389`.
