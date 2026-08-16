// Stage 7: reproduce AcceleratorBuffer's host->device transfer with RAW Vulkan on
// scopehal's device, with each ingredient toggleable, to find the exact trigger.
//
//   --pinnedtype N   memory type for the CPU-side buffer (default: g_vkPinnedMemoryType)
//   --localtype N    memory type for the GPU-side buffer (default: g_vkLocalMemoryType)
//   --noevent        skip the vkCmdSetEvent
//   --nocopy         skip the vkCmdCopyBuffer (submit an empty command buffer)
//
// Diagnostic only.

#include "scopehal.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace std;

#define VKCHK(x)                                                                \
	do                                                                          \
	{                                                                           \
		VkResult _r = (x);                                                      \
		if(_r != VK_SUCCESS)                                                    \
		{                                                                       \
			printf("FAIL: %s returned %d at line %d\n", #x, (int)_r, __LINE__); \
			return 1;                                                           \
		}                                                                       \
	} while(0)

int main(int argc, char** argv)
{
	Severity console_verbosity = Severity::ERROR;
	int pinnedType = -1;
	int localType = -1;
	bool useEvent = true;
	bool useCopy = true;

	for(int i = 1; i < argc; i++)
	{
		if(ParseLoggerArguments(i, argc, argv, console_verbosity))
			continue;
		string a(argv[i]);
		if(a == "--pinnedtype" && i + 1 < argc)
			pinnedType = atoi(argv[++i]);
		else if(a == "--localtype" && i + 1 < argc)
			localType = atoi(argv[++i]);
		else if(a == "--noevent")
			useEvent = false;
		else if(a == "--nocopy")
			useCopy = false;
	}
	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	if(!VulkanInit(false))
	{
		printf("VulkanInit failed\n");
		return 1;
	}

	VkDevice dev = **g_vkComputeDevice;
	VkPhysicalDevice phy = *(*g_vkComputePhysicalDevice);

	if(pinnedType < 0)
		pinnedType = g_vkPinnedMemoryType;
	if(localType < 0)
		localType = g_vkLocalMemoryType;

	printf("scopehal chose: pinned type %u, local type %u\n",
		g_vkPinnedMemoryType, g_vkLocalMemoryType);
	printf("this run using: pinned type %d, local type %d, event=%d copy=%d\n",
		pinnedType, localType, (int)useEvent, (int)useCopy);

	VkPhysicalDeviceMemoryProperties mprops;
	vkGetPhysicalDeviceMemoryProperties(phy, &mprops);
	printf("memory types (%u):\n", mprops.memoryTypeCount);
	for(uint32_t i = 0; i < mprops.memoryTypeCount; i++)
	{
		VkMemoryPropertyFlags f = mprops.memoryTypes[i].propertyFlags;
		printf("  [%u] heap %u flags 0x%02x %s%s%s%s\n", i, mprops.memoryTypes[i].heapIndex, f,
			(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
			(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
			(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
			(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "");
	}

	const size_t npoints = 4096;
	const VkDeviceSize bufSize = npoints * sizeof(float);

	// Exactly AcceleratorBuffer's usage flags
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = bufSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer cpuBuf, gpuBuf;
	VKCHK(vkCreateBuffer(dev, &bci, nullptr, &cpuBuf));
	VKCHK(vkCreateBuffer(dev, &bci, nullptr, &gpuBuf));

	VkMemoryRequirements creq, greq;
	vkGetBufferMemoryRequirements(dev, cpuBuf, &creq);
	vkGetBufferMemoryRequirements(dev, gpuBuf, &greq);
	printf("cpu buffer memoryTypeBits = 0x%08x, size %zu\n", creq.memoryTypeBits, (size_t)creq.size);
	printf("gpu buffer memoryTypeBits = 0x%08x, size %zu\n", greq.memoryTypeBits, (size_t)greq.size);
	printf("  pinned type %d allowed for cpu buffer: %s\n", pinnedType,
		(creq.memoryTypeBits & (1u << pinnedType)) ? "YES" : "*** NO ***");
	printf("  local type %d allowed for gpu buffer: %s\n", localType,
		(greq.memoryTypeBits & (1u << localType)) ? "YES" : "*** NO ***");

	VkMemoryAllocateInfo cmai = {};
	cmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	cmai.allocationSize = creq.size;
	cmai.memoryTypeIndex = pinnedType;
	VkDeviceMemory cpuMem;
	VKCHK(vkAllocateMemory(dev, &cmai, nullptr, &cpuMem));
	VKCHK(vkBindBufferMemory(dev, cpuBuf, cpuMem, 0));

	VkMemoryAllocateInfo gmai = {};
	gmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	gmai.allocationSize = greq.size;
	gmai.memoryTypeIndex = localType;
	VkDeviceMemory gpuMem;
	VKCHK(vkAllocateMemory(dev, &gmai, nullptr, &gpuMem));
	VKCHK(vkBindBufferMemory(dev, gpuBuf, gpuMem, 0));

	void* mapped = nullptr;
	VKCHK(vkMapMemory(dev, cpuMem, 0, creq.size, 0, &mapped));
	float* fp = (float*)mapped;
	for(size_t i = 0; i < npoints; i++)
		fp[i] = (float)i;

	auto q = g_vkTransferQueue;
	uint32_t qfi = q->GetQueue()->m_family;
	VkQueue queue = **(q->GetQueue()->GetQueue());
	printf("transfer queue family %u\n", qfi);

	VkCommandPoolCreateInfo cpoolci = {};
	cpoolci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpoolci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpoolci.queueFamilyIndex = qfi;
	VkCommandPool cpool;
	VKCHK(vkCreateCommandPool(dev, &cpoolci, nullptr, &cpool));

	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = cpool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cbuf;
	VKCHK(vkAllocateCommandBuffers(dev, &cbai, &cbuf));

	VkEvent evt = VK_NULL_HANDLE;
	if(useEvent)
	{
		VkEventCreateInfo eci = {};
		eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
		VKCHK(vkCreateEvent(dev, &eci, nullptr, &evt));
	}

	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VKCHK(vkBeginCommandBuffer(cbuf, &cbbi));
	if(useCopy)
	{
		VkBufferCopy region = {};
		region.size = bufSize;
		vkCmdCopyBuffer(cbuf, cpuBuf, gpuBuf, 1, &region);
	}
	if(useEvent)
		vkCmdSetEvent(cbuf, evt, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VKCHK(vkEndCommandBuffer(cbuf));

	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence;
	VKCHK(vkCreateFence(dev, &fci, nullptr, &fence));

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cbuf;

	printf("submitting...\n");
	fflush(stdout);
	VKCHK(vkQueueSubmit(queue, 1, &si, fence));

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000);
	if(wr != VK_SUCCESS)
	{
		printf("STAGE 7 FAIL: vkWaitForFences returned %d%s\n",
			(int)wr, wr == VK_ERROR_DEVICE_LOST ? " (VK_ERROR_DEVICE_LOST)" : "");
		return 1;
	}
	printf("STAGE 7 PASS\n");
	return 0;
}
