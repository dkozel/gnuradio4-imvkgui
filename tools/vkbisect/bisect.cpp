// Bisect harness: walks from "plain scopehal compute" to "VkFFT" one step at a time,
// to find which element faults the GPU. Diagnostic only -- not a project deliverable.
//
// Stages, each independently selectable so a fault in one does not mask the others:
//   1  scopehal ComputePipeline running AddFilter.spv (no VkFFT anywhere)
//   2  scopehal ComputePipeline running ComplexRectangularWindow.spv
//   3  VkFFT plan only
//
// Usage: ./bisect [stage ...]   (default: all)

#include "scopehal.h"
#include "ComputePipeline.h"
#include "AcceleratorBuffer.h"
#include "VulkanFFTPlan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace std;

static bool Stage1_AddFilter();
static bool Stage2_ComplexWindow();
static bool Stage3_VkFFT();
static bool Stage5_Blocking();
static bool Stage6_BufferOnly();

static shared_ptr<QueueHandle> g_queue;
static unique_ptr<vk::raii::CommandPool> g_pool;
static unique_ptr<vk::raii::CommandBuffer> g_cmdBuf;

static void MakeCmdBuf()
{
	g_queue = g_vkQueueManager->GetQueueFromPool(QueueManager::QUEUE_POOL_FILTER, "bisect");
	vk::CommandPoolCreateInfo poolInfo(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		g_queue->GetQueue()->m_family);
	g_pool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);

	vk::CommandBufferAllocateInfo bufinfo(**g_pool, vk::CommandBufferLevel::ePrimary, 1);
	g_cmdBuf = make_unique<vk::raii::CommandBuffer>(
		std::move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));
	LogNotice("Command buffer on queue family %d\n", (int)g_queue->GetQueue()->m_family);
}

//======================================================================================
// Stage 1: the simplest possible scopehal compute dispatch. No VkFFT, no filter graph.
//======================================================================================
static bool Stage1_AddFilter()
{
	LogNotice("STAGE 1: scopehal ComputePipeline + AddFilter.spv\n");
	LogIndenter li;

	const size_t npoints = 4096;

	AcceleratorBuffer<float> inP, inN, dout;
	inP.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	inP.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	inN.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	inN.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	dout.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	dout.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);

	inP.resize(npoints);
	inN.resize(npoints);
	dout.resize(npoints);

	inP.PrepareForCpuAccess();
	inN.PrepareForCpuAccess();
	for(size_t i=0; i<npoints; i++)
	{
		inP[i] = (float)i;
		inN[i] = (float)i * 2;
	}
	inP.MarkModifiedFromCpu();
	inN.MarkModifiedFromCpu();

	ComputePipeline pipe("shaders/AddFilter.spv", 3, sizeof(uint32_t));

	g_cmdBuf->begin({});
	pipe.BindBufferNonblocking(0, inP, *g_cmdBuf);
	pipe.BindBufferNonblocking(1, inN, *g_cmdBuf);
	pipe.BindBufferNonblocking(2, dout, *g_cmdBuf, true);
	uint32_t size = npoints;
	pipe.Dispatch(*g_cmdBuf, size, GetComputeBlockCount(npoints, 64));
	dout.MarkModifiedFromGpu();
	g_cmdBuf->end();

	LogNotice("submitting...\n");
	g_queue->SubmitAndBlock(*g_cmdBuf);
	LogNotice("submit returned cleanly\n");

	dout.PrepareForCpuAccess();
	size_t bad = 0;
	for(size_t i=0; i<npoints; i++)
	{
		float expect = (float)i * 3;
		if(fabs(dout[i] - expect) > 1e-3)
		{
			if(bad < 5)
				LogNotice("mismatch at %zu: got %f want %f\n", i, dout[i], expect);
			bad++;
		}
	}
	if(bad)
	{
		LogNotice("STAGE 1 FAIL (%zu mismatches)\n", bad);
		return false;
	}
	LogNotice("STAGE 1 PASS\n");
	return true;
}

//======================================================================================
// Stage 2: the window shader the FFT filter actually runs, in isolation.
//======================================================================================
static bool Stage2_ComplexWindow()
{
	LogNotice("STAGE 2: scopehal ComputePipeline + ComplexRectangularWindow.spv\n");
	LogIndenter li;

	const size_t npoints = 4096;

	AcceleratorBuffer<float> inI, inQ, dout;
	for(auto p : {&inI, &inQ, &dout})
	{
		p->SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
		p->SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	}
	inI.resize(npoints);
	inQ.resize(npoints);
	dout.resize(npoints * 2);

	inI.PrepareForCpuAccess();
	inQ.PrepareForCpuAccess();
	for(size_t i=0; i<npoints; i++)
	{
		inI[i] = cos(0.1 * i);
		inQ[i] = sin(0.1 * i);
	}
	inI.MarkModifiedFromCpu();
	inQ.MarkModifiedFromCpu();

	struct WindowConsts
	{
		uint32_t numActualSamples;
		uint32_t npoints;
		float scale;
	} consts;
	consts.numActualSamples = npoints;
	consts.npoints = npoints;
	consts.scale = 1.0f;

	ComputePipeline pipe("shaders/ComplexRectangularWindow.spv", 3, sizeof(WindowConsts));

	g_cmdBuf->begin({});
	pipe.BindBufferNonblocking(0, inI, *g_cmdBuf);
	pipe.BindBufferNonblocking(1, inQ, *g_cmdBuf);
	pipe.BindBufferNonblocking(2, dout, *g_cmdBuf, true);
	pipe.Dispatch(*g_cmdBuf, consts, GetComputeBlockCount(npoints, 64));
	dout.MarkModifiedFromGpu();
	g_cmdBuf->end();

	LogNotice("submitting...\n");
	g_queue->SubmitAndBlock(*g_cmdBuf);
	LogNotice("submit returned cleanly\n");

	dout.PrepareForCpuAccess();
	LogNotice("first outputs: %f %f %f %f\n", dout[0], dout[1], dout[2], dout[3]);
	LogNotice("STAGE 2 PASS\n");
	return true;
}

//======================================================================================
// Stage 3: VkFFT on its own, with no scopehal shader in the same command buffer.
//======================================================================================
static bool Stage3_VkFFT()
{
	LogNotice("STAGE 3: VkFFT plan only\n");
	LogIndenter li;

	const size_t npoints = 4096;

	AcceleratorBuffer<float> din, dout;
	for(auto p : {&din, &dout})
	{
		p->SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
		p->SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	}
	din.resize(npoints * 2);
	dout.resize(npoints * 2);

	din.PrepareForCpuAccess();
	for(size_t i=0; i<npoints; i++)
	{
		din[i*2]   = cos(0.1 * i);
		din[i*2+1] = sin(0.1 * i);
	}
	din.MarkModifiedFromCpu();

	LogNotice("creating plan...\n");
	VulkanFFTPlan plan(npoints, npoints, VulkanFFTPlan::DIRECTION_FORWARD, 1, VulkanFFTPlan::TYPE_COMPLEX);
	LogNotice("plan created\n");

	g_cmdBuf->begin({});
	plan.AppendForward(din, dout, *g_cmdBuf);
	g_cmdBuf->end();

	LogNotice("submitting...\n");
	g_queue->SubmitAndBlock(*g_cmdBuf);
	LogNotice("submit returned cleanly\n");

	dout.PrepareForCpuAccess();
	LogNotice("first outputs: %f %f %f %f\n", dout[0], dout[1], dout[2], dout[3]);
	LogNotice("STAGE 3 PASS\n");
	return true;
}


//======================================================================================
// Stage 5: identical to stage 1 but with the BLOCKING BindBuffer(), so no copyBuffer /
// setEvent is recorded into our command buffer. Splits AcceleratorBuffer's nonblocking
// transfer path from the rest.
//======================================================================================
static bool Stage5_Blocking()
{
	LogNotice("STAGE 5: ComputePipeline + AddFilter.spv, BLOCKING BindBuffer\n");
	LogIndenter li;

	const size_t npoints = 4096;

	AcceleratorBuffer<float> inP, inN, dout;
	for(auto p : {&inP, &inN, &dout})
	{
		p->SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
		p->SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	}
	inP.resize(npoints);
	inN.resize(npoints);
	dout.resize(npoints);

	inP.PrepareForCpuAccess();
	inN.PrepareForCpuAccess();
	for(size_t i=0; i<npoints; i++)
	{
		inP[i] = (float)i;
		inN[i] = (float)i * 2;
	}
	inP.MarkModifiedFromCpu();
	inN.MarkModifiedFromCpu();

	ComputePipeline pipe("shaders/AddFilter.spv", 3, sizeof(uint32_t));

	//Blocking binds happen OUTSIDE the command buffer
	LogNotice("transfer queue family = %d\n", (int)g_vkTransferQueue->GetQueue()->m_family);
	LogNotice("about to BindBuffer(0) [triggers DeferredInit + blocking CopyToGpu]\n");
	pipe.BindBuffer(0, inP);
	LogNotice("BindBuffer(0) ok\n");
	pipe.BindBuffer(1, inN);
	LogNotice("BindBuffer(1) ok\n");
	pipe.BindBuffer(2, dout, true);
	LogNotice("BindBuffer(2) ok\n");

	g_cmdBuf->begin({});
	uint32_t size = npoints;
	pipe.Dispatch(*g_cmdBuf, size, GetComputeBlockCount(npoints, 64));
	dout.MarkModifiedFromGpu();
	g_cmdBuf->end();

	LogNotice("submitting...\n");
	g_queue->SubmitAndBlock(*g_cmdBuf);
	LogNotice("submit returned cleanly\n");

	dout.PrepareForCpuAccess();
	size_t bad = 0;
	for(size_t i=0; i<npoints; i++)
	{
		if(fabs(dout[i] - (float)i*3) > 1e-3)
			bad++;
	}
	LogNotice("STAGE 5 %s (%zu mismatches)\n", bad ? "FAIL" : "PASS", bad);
	return bad == 0;
}


//======================================================================================
// Stage 6: AcceleratorBuffer ONLY. No ComputePipeline, no shader, no dispatch.
// Just a host->device blocking copy through scopehal's transfer machinery.
//======================================================================================
static bool Stage6_BufferOnly()
{
	LogNotice("STAGE 6: AcceleratorBuffer blocking host->device copy, no shader at all\n");
	LogIndenter li;

	const size_t npoints = 4096;

	AcceleratorBuffer<float> buf;
	buf.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	buf.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	LogNotice("resizing...\n");
	buf.resize(npoints);

	buf.PrepareForCpuAccess();
	for(size_t i=0; i<npoints; i++)
		buf[i] = (float)i;
	buf.MarkModifiedFromCpu();

	LogNotice("calling PrepareForGpuAccess (blocking CopyToGpu)...\n");
	buf.PrepareForGpuAccess(false);
	LogNotice("PrepareForGpuAccess returned\n");

	LogNotice("calling PrepareForCpuAccess (blocking CopyToCpu)...\n");
	buf.MarkModifiedFromGpu();
	buf.PrepareForCpuAccess();
	LogNotice("PrepareForCpuAccess returned\n");

	LogNotice("STAGE 6 PASS\n");
	return true;
}

int main(int argc, char** argv)
{
	Severity console_verbosity = Severity::NOTICE;
	set<int> stages;
	for(int i=1; i<argc; i++)
	{
		if(ParseLoggerArguments(i, argc, argv, console_verbosity))
			continue;
		stages.insert(atoi(argv[i]));
	}
	if(stages.empty())
		stages = {1, 2, 3};

	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	if(!VulkanInit(false))
	{
		LogError("VulkanInit failed\n");
		return 1;
	}

	MakeCmdBuf();

	int rc = 0;
	for(int s : stages)
	{
		try
		{
			bool ok = false;
			switch(s)
			{
				case 1: ok = Stage1_AddFilter(); break;
				case 2: ok = Stage2_ComplexWindow(); break;
				case 3: ok = Stage3_VkFFT(); break;
				case 5: ok = Stage5_Blocking(); break;
				case 6: ok = Stage6_BufferOnly(); break;
				default: LogError("unknown stage %d\n", s); continue;
			}
			if(!ok)
				rc = 1;
		}
		catch(const vk::DeviceLostError& e)
		{
			LogError("STAGE %d: DEVICE LOST (%s)\n", s, e.what());
			rc = 1;
			break;
		}
		catch(const std::exception& e)
		{
			LogError("STAGE %d: exception (%s)\n", s, e.what());
			rc = 1;
		}
	}

	g_cmdBuf.reset();
	g_pool.reset();
	g_queue = nullptr;
	ScopehalStaticCleanup();
	return rc;
}
