/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of ScopeHistory
 */
#ifndef ScopeHistory_h
#define ScopeHistory_h

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

/**
	@brief One port's recent samples, as a ring the render thread reads backwards from

	@par Why this is not the IqRing

	detail::IqRing drops the @b newest sample when it is full, which is the only correct policy
	for a display that must never stall a flowgraph. A scope's pre-trigger history has to drop the
	@b oldest, because the samples it exists to keep are the ones just before whatever is about to
	happen. That is the opposite ring, not a tunable of the same one, so there are two.

	@par Why this is not a template

	std::complex<float> is two adjacent floats - asserted at qa_IqLayout.cpp:66 - so interleaved
	complex data and real data differ only by a stride. Storing floats with a component count
	makes the drain a memcpy and confines the sample type to two places in the whole design: the
	IqRing inside the sink, and one reinterpret_cast at the call to Append().

	Deinterleaving on the way in would cost the same memory and about twice the drain bandwidth,
	in a scatter that does not vectorise, to save a strided read on the two paths that are already
	rare: the trigger scan reads one component, and the record copy runs once per acquisition.

	@par Coordinates

	Positions are @b stream indices: monotonic, never reset, counted from the first sample this
	object ever saw. notes/time-domain-plan.md section 1 is the argument for why a display's time
	base must be a coordinate of this kind and never an instrumentation counter. End() therefore
	keeps counting across Clear(), which is what makes a discontinuity marker comparable against
	positions recorded before it.

	@par Threading

	Render thread only. Nothing here is synchronised; it sits downstream of the IqRing, which is
	where the thread boundary actually is.
 */
class ScopeHistory
{
public:
	ScopeHistory()
		: m_depth(0)
		, m_comps(1)
		, m_filled(0)
		, m_end(0)
	{}

	/**
		@brief Resizes the ring, discarding its contents

		@param depthSamples		Complex or real samples held, not floats
		@param comps			1 for a real port, 2 for an interleaved complex one

		End() is preserved: a resize is not a rewind, and letting it look like one would make
		every position recorded before it compare wrongly.
	 */
	void Resize(std::size_t depthSamples, std::size_t comps)
	{
		if(comps < 1)
			comps = 1;

		if((depthSamples == m_depth) && (comps == m_comps))
			return;

		m_depth = depthSamples;
		m_comps = comps;
		m_buf.assign(m_depth * m_comps, 0.0f);
		m_filled = 0;
	}

	std::size_t GetDepth() const
	{ return m_depth; }

	std::size_t GetComponents() const
	{ return m_comps; }

	///@brief Stream index one past the newest sample held
	std::uint64_t End() const
	{ return m_end; }

	///@brief Stream index of the oldest sample still held. Equals End() when empty.
	std::uint64_t Begin() const
	{ return m_end - m_filled; }

	///@brief Samples currently resident, saturating at the depth
	std::uint64_t GetCount() const
	{ return m_filled; }

	/**
		@brief Discards everything held without rewinding the stream coordinate

		Begin() becomes End(). Used when alignment between ports is lost and the only honest
		recovery is to start again; see ScopeCapture::Resync().
	 */
	void Clear()
	{ m_filled = 0; }

	/**
		@brief Appends interleaved samples, overwriting the oldest

		@param in	Floats, a whole multiple of GetComponents() of them

		Samples beyond the depth in a single call are discarded from the front rather than
		wrapping over themselves, which costs one min and removes the only way a caller can make
		Append() do O(n) work for data that could never have been read back.
	 */
	void Append(std::span<const float> in)
	{
		if((m_depth == 0) || in.empty())
			return;

		std::size_t nsamples = in.size() / m_comps;
		if(nsamples == 0)
			return;

		//Anything older than the newest m_depth samples of this call is overwritten before
		//anyone could read it. Skip it rather than memcpy it and then bury it.
		if(nsamples > m_depth)
		{
			const std::size_t drop = nsamples - m_depth;
			m_end += drop;
			in = in.subspan(drop * m_comps, m_depth * m_comps);
			nsamples = m_depth;
		}

		const std::size_t head = static_cast<std::size_t>(m_end % m_depth);
		const std::size_t first = std::min(nsamples, m_depth - head);

		std::memcpy(&m_buf[head * m_comps], in.data(), first * m_comps * sizeof(float));
		if(nsamples > first)
		{
			std::memcpy(&m_buf[0], in.data() + first * m_comps,
				(nsamples - first) * m_comps * sizeof(float));
		}

		m_end += nsamples;
		m_filled = std::min<std::uint64_t>(m_filled + nsamples, m_depth);
	}

	/**
		@brief Copies one component of [first, first+n) into @a out

		@param first	Stream index of the first sample wanted
		@param n		Samples wanted
		@param comp		Component index, 0 for I or a real value and 1 for Q
		@param out		Destination, exactly @a n floats

		@return False if any of the range is not resident, in which case @a out is untouched

		All or nothing on purpose. A record with a hole in it is not a degraded picture of the
		signal, it is a picture of a different signal, and a scope that draws one is worse than a
		scope that admits it missed the acquisition.
	 */
	bool Gather(std::uint64_t first, std::size_t n, std::size_t comp, std::span<float> out) const
	{
		if(!Resident(first, n) || (comp >= m_comps) || (out.size() < n))
			return false;

		std::size_t idx = static_cast<std::size_t>(first % m_depth);
		for(std::size_t i = 0; i < n; i++)
		{
			out[i] = m_buf[idx * m_comps + comp];
			idx++;
			if(idx == m_depth)
				idx = 0;
		}
		return true;
	}

	/**
		@brief Copies |I + jQ|^2 of [first, first+n) into @a out

		@return False if the range is not resident or this is not a complex history

		Squared rather than the magnitude itself because that is what the trigger scan wants:
		squaring is monotonic on non-negative values, so comparing against level^2 gives exactly
		the same crossings as comparing the magnitude against level, and the square roots are then
		needed only for the two samples bracketing the one crossing that is reported.
	 */
	bool GatherMagSquared(std::uint64_t first, std::size_t n, std::span<float> out) const
	{
		if(!Resident(first, n) || (m_comps < 2) || (out.size() < n))
			return false;

		std::size_t idx = static_cast<std::size_t>(first % m_depth);
		for(std::size_t i = 0; i < n; i++)
		{
			const float re = m_buf[idx * m_comps + 0];
			const float im = m_buf[idx * m_comps + 1];
			out[i] = re*re + im*im;
			idx++;
			if(idx == m_depth)
				idx = 0;
		}
		return true;
	}

	///@brief True if every sample of [first, first+n) is still held
	bool Resident(std::uint64_t first, std::size_t n) const
	{
		if((m_depth == 0) || (n == 0))
			return false;
		if(first < Begin())
			return false;
		return (first + n) <= m_end;
	}

protected:
	///@brief m_depth * m_comps floats, interleaved, indexed by (sample % m_depth) * m_comps + comp
	std::vector<float> m_buf;

	std::size_t m_depth;
	std::size_t m_comps;

	///@brief Samples resident, saturating at m_depth
	std::uint64_t m_filled;

	///@brief Stream index one past the newest sample. Monotonic; survives Clear() and Resize().
	std::uint64_t m_end;
};

#endif
