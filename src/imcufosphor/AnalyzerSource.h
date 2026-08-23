/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of PlaybackStats, BlockSpan and IAnalyzerSource
 */
#ifndef AnalyzerSource_h
#define AnalyzerSource_h

#include "RowHistory.h"
#include "SpectrumDensity.h"

#include <scopeprotocols/Waterfall.h>

/**
	@brief Where playback time goes, accumulated since the last ResetStats()

	Split this way because the three are fixed by different things and are fixed
	differently: acquisition is host work bounded by the file and the sample converter,
	recording is CPU time building command buffers and scales with the number of dispatches,
	and submit is wall clock waiting on the GPU. Only the last is a GPU cost. Lumping them
	together makes it impossible to tell an overhead-bound pipeline from a compute-bound one,
	which is the entire question at hand.
 */
struct PlaybackStats
{
	///@brief Seconds spent getting samples in, however they arrive
	double acquireSec = 0;

	///@brief Seconds recording command buffers, i.e. in Filter::Refresh()
	double recordSec = 0;

	///@brief Seconds blocked in SubmitAndBlock()
	double submitSec = 0;

	///@brief Transforms executed
	int64_t spectra = 0;

	///@brief Calls to SubmitAndBlock()
	int64_t submits = 0;
};

/**
	@brief Sample range covered by one acquisition block, in both coordinate systems

	Recording coordinates are what SigMF annotations and the wall clock speak; stream
	coordinates are monotonic and are what elapsed durations must be measured in. See RowMark
	for the full taxonomy and for why the ingest statistics counter is not either of them.

	The recording range wraps: a block that straddles the end of the file during looped
	playback has a recordingEnd below its recordingStart. Callers converting this to a time
	or querying annotations over it have to handle that, which is precisely why the stream
	range is here beside it.

	A live stream has no recording coordinate, so a source fed from a port sets both pairs to
	the same values rather than inventing a file position.
 */
struct BlockSpan
{
	int64_t streamStart = 0;
	int64_t streamEnd = 0;
	int64_t recordingStart = 0;
	int64_t recordingEnd = 0;
};

/**
	@brief What the display needs from whatever is producing spectra

	AnalyzerPane and the areas under it care about four things: the two filters whose GPU
	buffers they tone map, which samples went into each waterfall row, and how fast samples
	arrive. They do not care whether those came from a file being played back or from a live
	flowgraph port, and before this interface existed they were forced to, because the pane
	took a PlayerSession and reached through it to a SigMFSource.

	Deliberately narrow. Everything specific to playing a recording - the play cursor, the
	wall clock, annotations, seeking - stays on PlayerSession, and the pane takes those
	separately through SetAnnotationSource() when they exist at all.
 */
class IAnalyzerSource
{
public:
	virtual ~IAnalyzerSource() = default;

	///@brief The density map filter, or null if this source does not compute one
	virtual SpectrumDensity* GetDensity() = 0;

	///@brief The waterfall filter, or null if this source does not compute one
	virtual Waterfall* GetWaterfall() = 0;

	///@brief Which samples went into each waterfall row
	virtual RowHistory& GetRowHistory() = 0;

	///@brief Sample rate of the data currently flowing, in Hz. Zero if not yet known.
	virtual double GetSampleRate() const = 0;

	/**
		@brief Sample range of the most recently processed block

		The spectrum shows this block, so anything drawn over the spectrum - a timestamp
		readout, an annotation span, a marker - is placed against this range rather than
		against a play cursor, which has already moved on by the time the frame is drawn.
	 */
	virtual const BlockSpan& GetCurrentBlock() const = 0;
};

#endif
