/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of IqRing
 */
#ifndef GR_IMCUFOSPHOR_IQ_RING_HPP
#define GR_IMCUFOSPHOR_IQ_RING_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>

#include <gnuradio-4.0/CircularBuffer.hpp>

namespace gr::imcufosphor::detail {

/**
	@brief The scheduler-to-renderer handoff, with drop accounting

	Every display sink has the same problem: samples arrive on a scheduler thread in whatever
	sized chunks the graph happens to deliver, and are consumed on a render thread in
	GPU-block-sized pieces at frame rate. This is the buffer in between, factored out so the
	three sinks share one implementation of it rather than three that drift apart.

	@par Lock-free, and drop rather than block

	A lock held across a megasample copy shows up directly as scheduler jitter, so this is
	gr::CircularBuffer - the same lock-free SPSC primitive gnuradio4's own StreamingPoller is
	built on. When the reader falls behind, Push() discards what will not fit and counts it: a
	display must never be able to stall a flowgraph, and a graph running with no window at all
	is a supported mode in which this degenerates to a bit bucket.

	@par Threading

	Push() and the counters are the writer side; Available() and WithBlock() are the reader
	side. Exactly one thread may do each. Resize() is neither, and may only be called before
	either side starts - i.e. from the block's start().
 */
template<typename T>
class IqRing
{
public:
	/**
		@brief Sets the capacity, discarding anything held

		CircularBuffer rounds up to a page, so the result is at least the requested depth and
		usually more.
	 */
	void Resize(std::size_t depth)
	{
		//The buffer first, then handles onto it. The old buffer stays alive until the old
		//handles are replaced, so nothing is left pointing at freed storage.
		m_ring = gr::CircularBuffer<T>(std::max<std::size_t>(depth, 1UZ));
		m_writer = m_ring.new_writer();
		m_reader = m_ring.new_reader();

		m_pushed.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
	}

	///@brief Capacity in samples
	[[nodiscard]] std::size_t Capacity() const
	{ return m_ring.size(); }

	///@brief Samples that would fit right now. Writer thread only.
	[[nodiscard]] std::size_t Room() const
	{ return m_writer.available(); }

	/**
		@brief Copies what fits and reports how much that was, dropping nothing

		The backpressure counterpart to Push(): the caller consumes only what was accepted, so
		whatever did not fit stays in the port and the upstream block is throttled instead of
		its output being discarded. Correct for a file, wrong for a radio - see the sink's
		`backpressure` setting for which is which.
	 */
	[[nodiscard]] std::size_t PushUpTo(std::span<const T> samples)
	{
		const std::size_t n = samples.size();
		if(n == 0)
			return 0;

		const std::size_t want = std::min(n, m_writer.available());
		if(want == 0)
			return 0;

		auto span = m_writer.template tryReserve<gr::SpanReleasePolicy::ProcessNone>(want);
		if(span.empty())
			return 0;

		const std::size_t taken = std::min(want, span.size());
		std::copy_n(samples.begin(), taken, span.begin());
		span.publish(taken);

		m_pushed.fetch_add(taken, std::memory_order_relaxed);
		return taken;
	}

	/**
		@brief Copies what fits and drops the rest. Writer thread only.

		Never blocks and never partially refuses: the caller is expected to have consumed its
		whole input span regardless of what happened here.
	 */
	void Push(std::span<const T> samples)
	{
		const std::size_t n = samples.size();
		if(n == 0)
			return;

		const std::size_t room = m_writer.available();
		const std::size_t want = std::min(n, room);

		std::size_t taken = 0;
		if(want > 0)
		{
			auto span = m_writer.template tryReserve<gr::SpanReleasePolicy::ProcessNone>(want);
			if(!span.empty())
			{
				taken = std::min(want, span.size());
				std::copy_n(samples.begin(), taken, span.begin());
				span.publish(taken);
			}
		}

		m_pushed.fetch_add(taken, std::memory_order_relaxed);
		m_dropped.fetch_add(n - taken, std::memory_order_relaxed);
	}

	///@brief Samples ready to be read. Reader thread only.
	[[nodiscard]] std::size_t Available() const
	{ return m_reader.available(); }

	/**
		@brief Runs a callback over exactly @p n samples, then consumes them

		@return False if fewer than @p n samples are available, in which case the callback is
		        not run and nothing is consumed.

		Reader thread only. The callback form exists so the reader span cannot outlive the
		consume: the span points into the ring, and releasing it before the GPU upload has
		finished reading would let the writer overwrite data in flight.
	 */
	template<typename TFunc>
	bool WithBlock(std::size_t n, TFunc&& func)
	{
		if((n == 0) || (m_reader.available() < n))
			return false;

		auto span = m_reader.template get<gr::SpanReleasePolicy::ProcessNone>(n);
		if(span.size() < n)
			return false;

		func(std::span<const T>(span.data(), n));

		std::ignore = span.consume(n);
		return true;
	}

	///@brief Samples accepted since the last Resize()
	[[nodiscard]] std::uint64_t Pushed() const
	{ return m_pushed.load(std::memory_order_relaxed); }

	///@brief Samples discarded because the reader was behind
	[[nodiscard]] std::uint64_t Dropped() const
	{ return m_dropped.load(std::memory_order_relaxed); }

private:
	gr::CircularBuffer<T> m_ring{1024UZ};
	decltype(m_ring.new_writer()) m_writer = m_ring.new_writer();
	decltype(m_ring.new_reader()) m_reader = m_ring.new_reader();

	std::atomic<std::uint64_t> m_pushed{0};
	std::atomic<std::uint64_t> m_dropped{0};
};

} // namespace gr::imcufosphor::detail

#endif
