# Time domain and sample coordinates — as built

Split from `annotation-overlay-plan.md`, which shares exactly one prerequisite with this work
(`RowHistory` carrying two coordinates) and nothing else. The overlay needs recording sample
indices and no clock at all; the clock needs no annotations. Serialising an ISO 8601 parser in
front of drawing a box would have been a mistake.

Implemented and verified against the working tree at `d56ccb4` plus the uncommitted changes.

---

## 1. The four sample counters

They are not interchangeable, and two of them were conflated before this change.

| Coordinate | Where it lives | Wraps? | Resettable? | What it is for |
|---|---|---|---|---|
| **recording** | `SigMFSource::m_playCursor`, `RowMark::recordingStart`, `BlockSpan::recording*` | yes, on every loop | yes, via `SeekToSample()` | SigMF annotations, captures, the wall clock |
| **stream** | `SigMFSource::m_samplesPlayed`, `RowMark::streamStart`, `BlockSpan::stream*` | never | never | elapsed durations, the waterfall's time axis |
| **ingest statistic** | `SigMFSource::m_samplesDelivered` | never | **yes**, `ResetIngestStats()` | benchmarking, and nothing else |
| **wall clock** | derived, `RecordingClock` | — | — | timestamps and readouts |

**The rule this exists to enforce: an instrumentation counter must never be a time base.**
`PlayerSession.cpp` previously pushed `GetSamplesDelivered()` into `RowHistory` as if it were
the stream coordinate. It behaves identically right up until someone adds a "reset stats"
button, at which point the waterfall's time axis jumps backwards. `m_samplesPlayed` is
deliberately declared in its own header section, away from the ingest accounting block, so the
next person to add a counter has to notice which kind they are adding.

## 2. `RecordingClock`

`RecordingClock.{h,cpp}`, built once at open, owned by `SigMFSource`, reachable as
`GetClock()`. `RecordingClock.h` depends on nothing but the standard library, so displays can
include it without dragging in libsigmf — the same boundary `annotation-overlay-plan.md` §A1
draws for `Annotation.h`.

### Pinned to recording coordinates, never to stream coordinates

Loop a three second file for a minute and a stream-pinned clock claims data exists at T+60 s
that the recording never contained. Time is a property of the samples, not of how many times
they have been replayed.

This also generalises with no special case: for a live source, recording and stream
coordinates are the same thing and the total sample count is simply unbounded.

### Capture-scoped

SigMF permits several captures, each with its own `core:datetime` and `core:sample_start`, and
the spec does not require them to be contiguous in time. Every recording in the demo dataset
has exactly one capture, which is precisely the reason to handle the general case now rather
than discover it later. A test with two captures 51 s apart confirms the gap survives.

### Never accumulate femtoseconds

`fs_per_sample` is an integer in scopehal. At 245.76 MS/s the true value is 4069010.4167
against a stored 4069010, so an accumulating implementation drifts ~0.37 ms per hour. Times
are recomputed from the sample index every call: one division, good to about a picosecond over
a day, because a double holds sample indices below 9e15 exactly. Verified — one hour of
samples at 245.76 MS/s lands on the second to nine decimal places.

### `SampleTime` is split, and honest

```cpp
struct SampleTime { time_t sec; int64_t fs; bool absolute; };
```

Split because int64 femtoseconds from the Unix epoch saturate 2 h 34 min after 1970. Same
field meanings as scopehal's `m_startTimestamp` / `m_startFemtoseconds`, so one can be
assigned onto the other.

`absolute` distinguishes `2025-07-22 18:16:03.487 UTC` from `+4.207 s`. `Format()` renders
them unmistakably differently, and the leading sign on the relative form is deliberate.

## 3. What the metadata actually contains

`core:datetime` was parsed by libsigmf and ignored by us; `m_startTimestamp` came from the
**data file's mtime**. Every waveform in the pipeline carried a fabricated timestamp.

The mtime fallback is retained for scopehal's waveform fields, which want *something*, and is
now forbidden from reaching any display. Anything user-facing asks `GetClock()` and is told
when the answer is not known.

Four datetime spellings appear across eight demo recordings, and the parser handles all of
them plus `+HHMM`, `+HH`, and a space in place of the `T`:

```
2025-09-17T06:45:29Z                 whole seconds, zulu
2025-07-22T18:16:03.0000000          seven fractional digits, NO timezone
2025-12-16T20:04:09.612882           microseconds, no timezone
2026-02-12T16:08:21.787343+00:00     explicit offset
```

A missing timezone is not RFC 3339 conformant but two of eight files omit it, so it is read as
UTC and warned about once at load — a recording actually taken elsewhere is now wrong by that
many hours and only the warning will say so.

`dect6.sigmf-meta` spells the key **`core::datetime`**, with two colons, so it parses as
absent. It is the no-absolute-time path's live test case, and it reports `RELATIVE ONLY`.

Note that libsigmf gives `CaptureT::datetime` as a bare `std::string` while every sibling
field is an `Optional<>`; absent and present-but-empty are the same value.

## 4. Where the counters live, and where they do not

Sample counters are maintained where samples and rows are produced — `SigMFSource` and
`PlayerSession` — and handed to the display areas. This is the same argument `RowHistory`
already makes for its own ownership: pushing at the point of truth means it happens exactly
once. A `WaterfallArea` that counted for itself would be a second counter to disagree with the
first, and a standalone area and a pane would disagree about what time it is.

`RowMark` holds both coordinates in one struct rather than two parallel vectors. Both fields
are written by one `Push()` and read together; two vectors kept in step by hand is a
desynchronisation waiting to happen for no benefit. (This is *not* the argument against
`m_maxEndPrefix` in the annotation plan §4, which genuinely is scanned independently.)

## 5. Files

| File | Status | Contents |
|------|--------|----------|
| `RecordingClock.{h,cpp}` | new | `SampleTime`, `RecordingClock`, ISO 8601 parser. No dependency beyond the standard library in the header. |
| `RowHistory.h` | rewritten | `RowMark` with both coordinates; `Push(stream, recording)`, `GetRow()`, `GetStreamSpan()`. |
| `SigMFSource.{h,cpp}` | edit | `m_samplesPlayed`, `GetSamplesPlayed()`, `m_clock`, `GetClock()`; clock built in `LoadMetadata()`; mtime demoted to a fallback for scopehal's fields only. |
| `PlayerSession.{h,cpp}` | edit | `BlockSpan`, `GetCurrentBlock()`, `m_groupStart` as a `RowMark`; block range captured either side of `AcquireData()`. |
| `WaterfallArea.{h,cpp}` | edit | `SetTimebase()` takes the clock; hover tooltip gives the row's time. |
| `AnalyzerPane.cpp` | edit | Passes the clock through. |
| `MainWindow.cpp` | edit | Status line readout. |

## 6. Two behaviours worth not mistaking for bugs

**The waterfall y axis and the readout disagree, on purpose.** Playing the 0.050 s
`tone_signal` shows an axis running to −9 s while the readout says `0.026 of 0.050 s into
recording`. The axis is stream time — elapsed playback across many loops — and the readout is
recording time. Both are right; they answer different questions.

**`Restart()` rewinds only the recording coordinate.** The stream coordinate is a count of
samples played and a restart does not un-play them. Zeroing it would make the time axis jump
backwards across the restart.

## 7. What this unblocks

Absolute-time waterfall axis labelling; seek-to-time; annotation tooltips that say when a
signal was seen; correlating two recordings (the two 2410 MHz demo files are 51 s apart and
the clock now knows it).

Still open: `GetSamplesDelivered()` remains public and still looks like a usable timebase to a
reader who has not read §1. Removing or renaming it is a separate change, since `wfbench`
reports it.
