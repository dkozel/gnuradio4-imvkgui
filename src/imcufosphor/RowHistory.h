/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of RowHistory
 */
#ifndef RowHistory_h
#define RowHistory_h

#include <cstdint>
#include <vector>

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
		if(depth == m_starts.size())
			return;

		m_starts.assign(depth, 0);
		m_writePtr = 0;
		m_count = 0;
	}

	size_t GetDepth() const
	{ return m_starts.size(); }

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

		@param sampleStart	Index of the first sample that contributed to this row
	 */
	void Push(int64_t sampleStart)
	{
		if(m_starts.empty())
			return;

		m_starts[m_writePtr] = sampleStart;
		m_writePtr = (m_writePtr + 1) % m_starts.size();
		if(m_count < m_starts.size())
			m_count++;
	}

	/**
		@brief First sample of the row @a age rows behind the newest

		@param age		Zero is the newest row
		@param out		Sample index, untouched if the row is not recorded

		@return False if that row has fallen out of the ring or was never written
	 */
	bool GetSampleStart(size_t age, int64_t& out) const
	{
		if( (age >= m_count) || m_starts.empty() )
			return false;

		//m_writePtr points at the next slot to write, so the newest row is one behind it
		size_t idx = (m_writePtr + m_starts.size() - 1 - age) % m_starts.size();
		out = m_starts[idx];
		return true;
	}

	/**
		@brief Sample span covered by the newest @a rows rows

		@return Zero if fewer than two rows are recorded, since one row spans nothing
			measurable and a caller dividing by this needs to notice
	 */
	int64_t GetSampleSpan(size_t rows) const
	{
		if(m_count < 2)
			return 0;

		size_t oldest = min(rows, m_count) - 1;

		int64_t newestStart = 0;
		int64_t oldestStart = 0;
		if(!GetSampleStart(0, newestStart) || !GetSampleStart(oldest, oldestStart))
			return 0;

		//Playback loops, so the cursor wraps to zero at the end of the recording and the
		//difference goes negative. Report nothing rather than a negative time span; the next
		//few rows will push the wrap out of the window.
		int64_t span = newestStart - oldestStart;
		return (span > 0) ? span : 0;
	}

protected:
	static size_t min(size_t a, size_t b)
	{ return (a < b) ? a : b; }

	///@brief First sample of each row, indexed by ring position
	std::vector<int64_t> m_starts;

	///@brief Next slot to write
	size_t m_writePtr;

	///@brief Rows written, saturating at the ring depth
	size_t m_count;
};

#endif
