/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of GpuTimer
 */

#include "GpuTimer.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

GpuTimer::GpuTimer(size_t maxMarks)
	: m_supported(false)
	, m_period(0)
	, m_maxMarks(maxMarks)
	, m_written(0)
	, m_samples(0)
{
	auto limits = g_vkComputePhysicalDevice->getProperties().limits;

	//A timestampPeriod of zero means the device cannot timestamp at all
	m_period = limits.timestampPeriod;
	if(m_period <= 0)
	{
		LogWarning("GpuTimer: device reports timestampPeriod 0, GPU timing unavailable\n");
		return;
	}

	//One extra query for the Begin() timestamp
	vk::QueryPoolCreateInfo info({}, vk::QueryType::eTimestamp, m_maxMarks + 1);
	m_pool = make_unique<vk::raii::QueryPool>(*g_vkComputeDevice, info);

	m_supported = true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Recording

void GpuTimer::Begin(vk::raii::CommandBuffer& cmdBuf)
{
	if(!m_supported)
		return;

	//Reset on the device rather than the host: VK_EXT_host_query_reset is not something
	//scopehal's device creation asks for, and the reset has to be ordered against the
	//timestamp writes anyway.
	cmdBuf.resetQueryPool(**m_pool, 0, m_maxMarks + 1);
	cmdBuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, **m_pool, 0);

	m_written = 1;
}

void GpuTimer::Mark(vk::raii::CommandBuffer& cmdBuf, const string& label)
{
	if(!m_supported)
		return;

	//Silently dropping marks would produce plausible but wrong stage times, so refuse loudly
	if(m_written > m_maxMarks)
	{
		LogWarning("GpuTimer: more than %zu marks in one command buffer, ignoring \"%s\"\n",
			m_maxMarks, label.c_str());
		return;
	}

	//Bottom of pipe, so the timestamp waits for the stage's work rather than only for the
	//commands to be issued
	cmdBuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, **m_pool, m_written);

	//Labels are fixed after the first pass. A caller that records a different set of stages
	//on different passes would be averaging unlike things.
	size_t stage = m_written - 1;
	if(stage < m_labels.size())
	{
		if(m_labels[stage] != label)
		{
			LogWarning("GpuTimer: stage %zu was \"%s\" and is now \"%s\"; averages will be meaningless\n",
				stage, m_labels[stage].c_str(), label.c_str());
		}
	}
	else
	{
		m_labels.push_back(label);
		m_accNs.push_back(0);
	}

	m_written++;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Readback

void GpuTimer::Collect()
{
	if(!m_supported || (m_written < 2))
		return;

	//Called through the C API deliberately: vulkan-hpp's getResults() return type has
	//changed shape across versions (ResultValue vs pair vs throwing), and this has to build
	//against whatever headers the system provides.
	vector<uint64_t> ticks(m_written);
	VkResult r = vkGetQueryPoolResults(
		**g_vkComputeDevice,
		**m_pool,
		0,
		m_written,
		ticks.size() * sizeof(uint64_t),
		ticks.data(),
		sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if(r != VK_SUCCESS)
	{
		LogWarning("GpuTimer: vkGetQueryPoolResults returned %d\n", static_cast<int>(r));
		return;
	}

	for(uint32_t i=1; i<m_written; i++)
		m_accNs[i-1] += (ticks[i] - ticks[i-1]) * m_period;

	m_samples++;
	m_written = 0;
}

double GpuTimer::GetAverageMs(size_t i) const
{
	if( (i >= m_accNs.size()) || (m_samples == 0) )
		return 0;

	return m_accNs[i] / m_samples / 1e6;
}

double GpuTimer::GetAverageTotalMs() const
{
	double sum = 0;
	for(size_t i=0; i<m_accNs.size(); i++)
		sum += GetAverageMs(i);
	return sum;
}
