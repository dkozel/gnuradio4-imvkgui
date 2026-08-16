/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of GpuTimer
 */
#ifndef GpuTimer_h
#define GpuTimer_h

#include "../../lib/scopehal/scopehal/scopehal.h"

#include <string>
#include <vector>

/**
	@brief Measures GPU time spent in each stage of a command buffer

	scopehal has NamedDebugRange for labelling regions so a capture tool can show them, but
	nothing that reports elapsed GPU time back to the program, and there is no query pool
	anywhere in the tree. Attributing throughput work needs numbers in the log, not a
	RenderDoc session.

	Usage, once per command buffer:

	@verbatim
	timer.Begin(cmdBuf);                //immediately after cmdBuf.begin()
	  ...record the window shader...
	timer.Mark(cmdBuf, "window");
	  ...record the FFT...
	timer.Mark(cmdBuf, "fft");
	cmdBuf.end();
	queue->SubmitAndBlock(cmdBuf);
	timer.Collect();                    //only once the submit has completed
	@endverbatim

	Timestamps are written at the bottom of the pipe, so each Mark() reports the interval
	since the previous one. Marks are cumulative across Collect() calls, so a benchmark can
	run thousands of blocks and read a stable average at the end.

	Collect() must not be called until the submit has completed. It passes WAIT to
	vkGetQueryPoolResults, so calling it early blocks rather than returning garbage, but
	that stalls the very pipeline being measured.
 */
class GpuTimer
{
public:
	GpuTimer(size_t maxMarks = 16);

	/**
		@brief True if the device and queue family can timestamp

		Timestamp support is not universal (notably llvmpipe, which we use as the Xid-32
		fallback device), so every caller has to be able to run without it.
	 */
	bool IsSupported() const
	{ return m_supported; }

	///@brief Resets the query pool and writes the start timestamp. Call right after begin().
	void Begin(vk::raii::CommandBuffer& cmdBuf);

	///@brief Writes a timestamp closing the stage named @a label
	void Mark(vk::raii::CommandBuffer& cmdBuf, const std::string& label);

	///@brief Reads back the timestamps and accumulates them. Call only after the submit completes.
	void Collect();

	///@brief Number of completed Collect() calls
	int64_t GetSampleCount() const
	{ return m_samples; }

	///@brief Stage names, in the order they were marked
	const std::vector<std::string>& GetLabels() const
	{ return m_labels; }

	///@brief Mean milliseconds spent in stage @a i across every Collect()
	double GetAverageMs(size_t i) const;

	///@brief Mean milliseconds from Begin() to the last mark
	double GetAverageTotalMs() const;

protected:
	bool m_supported;

	///@brief Nanoseconds per timestamp tick, from VkPhysicalDeviceLimits
	double m_period;

	size_t m_maxMarks;

	std::unique_ptr<vk::raii::QueryPool> m_pool;

	///@brief Timestamps written so far in the command buffer being recorded, including the start
	uint32_t m_written;

	std::vector<std::string> m_labels;

	///@brief Summed nanoseconds per stage, parallel to m_labels
	std::vector<double> m_accNs;

	int64_t m_samples;
};

#endif
