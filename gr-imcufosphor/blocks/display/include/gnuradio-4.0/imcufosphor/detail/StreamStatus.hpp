/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of StreamStatus
 */
#ifndef GR_IMCUFOSPHOR_STREAM_STATUS_HPP
#define GR_IMCUFOSPHOR_STREAM_STATUS_HPP

#include <atomic>
#include <cinttypes>
#include <cstdint>

#include "imgui.h"

#include <gnuradio-4.0/imcufosphor/detail/MetaFromTags.hpp>

namespace gr::imcufosphor::detail {

/**
	@brief What the stream has told us about itself, and what the display did with it

	Shared by all three sinks so that the meaning of each counter, and the way it is drawn, is
	decided once. The distinction the display exists to make visible is between two kinds of
	missing samples that look identical in a spectrum but mean opposite things:

	- @b dropped here: the render thread could not keep up, so the display skipped samples. By
	  design, expected at any real sample rate, and not a fault.
	- @b upstream: samples that never arrived. A genuine gap in the signal, and the spectrum is
	  lying about what was on the air during it.

	Written by the scheduler thread from processBulk(), read by the render thread in draw().
 */
struct StreamStatus
{
	///@brief Samples lost before this block saw them
	std::atomic<std::uint64_t> upstreamDropped{0};

	///@brief Set once the stream reports a receiver overflow, and never cleared
	std::atomic<bool> overflow{false};

	/**
		@brief Set by an end_of_stream tag

		Sinks freeze on this rather than clearing: when a stream ends, what is on screen is the
		only remaining view of it, and blanking destroys that.
	 */
	std::atomic<bool> endOfStream{false};

	///@brief Folds in what a block of tags reported. Scheduler thread.
	void Absorb(const TagMeta& meta)
	{
		if(meta.upstreamDropped.has_value())
			upstreamDropped.fetch_add(*meta.upstreamDropped, std::memory_order_relaxed);
		if(meta.overflow)
			overflow.store(true, std::memory_order_relaxed);
		if(meta.endOfStream)
			endOfStream.store(true, std::memory_order_release);
	}

	///@brief True once the stream has ended and the display should stop advancing
	[[nodiscard]] bool Frozen() const
	{ return endOfStream.load(std::memory_order_acquire); }
};

/**
	@brief Draws the one-line status readout common to every sink

	@param status	Stream-reported state
	@param pushed	Samples accepted into the handoff ring
	@param dropped	Samples the display declined because it was behind
	@param detail	Optional leading text, e.g. a row and spectrum count. May be null.
 */
inline void RenderStreamStatus(
	const StreamStatus& status,
	std::uint64_t pushed,
	std::uint64_t dropped,
	const char* detail)
{
	if(status.Frozen())
		ImGui::TextUnformatted("end of stream - display frozen");
	else if(detail != nullptr)
		ImGui::TextUnformatted(detail);
	else
		ImGui::TextUnformatted("running");

	const std::uint64_t total = pushed + dropped;
	ImGui::SameLine();
	ImGui::Text(" | shown %.1f%%",
		total ? (100.0 * static_cast<double>(pushed) / static_cast<double>(total)) : 0.0);

	const auto upstream = status.upstreamDropped.load(std::memory_order_relaxed);
	if(upstream > 0)
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
			" | %" PRIu64 " lost upstream", upstream);
	}

	if(status.overflow.load(std::memory_order_relaxed))
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), " | RX overflow");
	}
}

} // namespace gr::imcufosphor::detail

#endif
