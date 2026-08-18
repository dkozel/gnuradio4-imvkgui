/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of RowMark and RowHistory
 */
#ifndef RowHistory_h
#define RowHistory_h

#include <cstdint>
#include <vector>

/**
	@brief Where one waterfall row's samples came from, in both coordinate systems

	@par Why two numbers and not one

	There are four sample counters in this application and they are not interchangeable. Two of
	them belong here:

	- @b stream coordinates count samples handed downstream since playback started. Monotonic:
	  they survive looping and seeking, so a difference between two rows is always a real
	  elapsed duration. This is what a time axis span must be measured in.
	- @b recording coordinates index into the file, wrapping to zero every time playback loops.
	  This is what SigMF annotations and captures speak, and the only coordinate a wall clock
	  can be pinned to.

	The other two are the play cursor, which is recording coordinates by another name, and
	SigMFSource::GetSamplesDelivered(), which is an ingest statistic that ResetIngestStats()
	zeroes and must therefore never be used as a time base.

	Held as one struct rather than two parallel vectors because both fields are written by the
	same Push() and, in the overlay's row sweep, read together. Two vectors kept in step by
	hand is a desynchronisation waiting to happen for no benefit.
 */
struct RowMark
{
	///@brief Samples delivered since playback started, when this row's group began
	int64_t streamStart = 0;

	///@brief Index into the recording of this row's first sample
	int64_t recordingStart = 0;
};

/**
	@brief Which samples went into each waterfall row

	Upstream's waterfall has no notion of when a row happened. WaterfallWaveform exposes only
	GetWriteRow() and BumpWriteRow() (Waterfall.h:67-71), rows are acquisitions rather than
	sample indices, and the tone map maps row to screen with modular arithmetic. Without
	something like this the waterfall is a scrolling image that cannot be given a time axis
	or a timestamp readout at all. See DESIGN.md section 8.2.

	The obvious shortcut - row k from the top is k * blockSize samples back - holds only
	while the block size never changes and no acquisition is ever dropped. Recording the
	cursor position per row is barely more code and does not lie.

	@par Ownership

	DESIGN.md section 8.2 puts this in AnalyzerPane. It lives in PlayerSession instead,
	because that is where rows are actually produced: pushing at the point of truth means it
	is populated exactly once, and both a standalone WaterfallArea and a pane can read it. A
	pane-owned ring would have to be fed by whoever happened to be driving playback.

	The same argument decides where the sample counters live. They are maintained in
	SigMFSource and PlayerSession, where samples and rows are produced, and handed to the
	display areas; an area that counted for itself would be a second counter to disagree with
	the first.

	@par Sign

	Newest row is at the top of the display, so increasing sample index goes upward and age
	increases downward.
 */
class RowHistory
{
public:
	RowHistory()
		: m_writePtr(0)
		, m_count(0)
	{}

	/**
		@brief Resizes the ring, discarding its contents

		Depth should match the waterfall's row count. A mismatch does not corrupt anything,
		it just means the oldest rows on screen have no entry.
	 */
	void SetDepth(size_t depth)
	{
		if(depth == m_rows.size())
			return;

		m_rows.assign(depth, RowMark());
		m_writePtr = 0;
		m_count = 0;
	}

	size_t GetDepth() const
	{ return m_rows.size(); }

	///@brief Rows recorded since the last Clear(), saturating at the ring depth
	size_t GetCount() const
	{ return m_count; }

	void Clear()
	{
		m_writePtr = 0;
		m_count = 0;
	}

	/**
		@brief Records a row

		@param streamStart		Samples delivered when this row's group began, monotonic
		@param recordingStart	Index into the recording of this row's first sample

		Both coordinates, from one call, so they cannot disagree about which row they describe.
	 */
	void Push(int64_t streamStart, int64_t recordingStart)
	{
		if(m_rows.empty())
			return;

		m_rows[m_writePtr].streamStart = streamStart;
		m_rows[m_writePtr].recordingStart = recordingStart;
		m_writePtr = (m_writePtr + 1) % m_rows.size();
		if(m_count < m_rows.size())
			m_count++;
	}

	/**
		@brief Both coordinates of the row @a age rows behind the newest

		@param age		Zero is the newest row
		@param out		The row, untouched if it is not recorded

		@return False if that row has fallen out of the ring or was never written
	 */
	bool GetRow(size_t age, RowMark& out) const
	{
		if( (age >= m_count) || m_rows.empty() )
			return false;

		//m_writePtr points at the next slot to write, so the newest row is one behind it
		size_t idx = (m_writePtr + m_rows.size() - 1 - age) % m_rows.size();
		out = m_rows[idx];
		return true;
	}

	/**
		@brief Elapsed sample count across the newest @a rows rows

		In stream coordinates, so this is a true duration even when playback looped partway
		through the window. Measuring it in recording coordinates instead would go negative at
		every wrap, which is exactly the bug this coordinate exists to avoid.

		@return Zero if fewer than two rows are recorded, since one row spans nothing
			measurable and a caller dividing by this needs to notice
	 */
	int64_t GetStreamSpan(size_t rows) const
	{
		if(m_count < 2)
			return 0;

		size_t oldest = min(rows, m_count) - 1;

		RowMark newest;
		RowMark old;
		if(!GetRow(0, newest) || !GetRow(oldest, old))
			return 0;

		int64_t span = newest.streamStart - old.streamStart;
		return (span > 0) ? span : 0;
	}

protected:
	static size_t min(size_t a, size_t b)
	{ return (a < b) ? a : b; }

	///@brief Both coordinates of each row, indexed by ring position
	std::vector<RowMark> m_rows;

	///@brief Next slot to write
	size_t m_writePtr;

	///@brief Rows written, saturating at the ring depth
	size_t m_count;
};

#endif
