// Minimal reproducer for an NVIDIA Xid 32 (corrupted push buffer) / VK_ERROR_DEVICE_LOST.
//
//   Hardware: GeForce RTX 5060 Ti (Blackwell, device ID 0x2d04)
//   Driver:   580.173.02 (nvidia-driver-580-open), Ubuntu 24.04, loader 1.3.275
//
// Trigger, all three conditions required:
//   1. The logical device is created with queues in the graphics family (family 0)
//      AND in the async-compute family (family 2).
//   2. A command buffer allocated from a family 2 pool contains vkCmdSetEvent.
//   3. The stage mask of that vkCmdSetEvent is VK_PIPELINE_STAGE_TRANSFER_BIT.
//
// The command buffer contains nothing else -- no dispatch, no copy, no bound resources.
// Khronos validation (1.3.275) reports no errors on the recording or submission path.
//
// Result: vkWaitForFences returns VK_ERROR_DEVICE_LOST and the kernel logs
//   NVRM: Xid (PCI:0000:01:00): 32, pid=..., name=xid32_repro, channel 0x...
//
// Passes if ANY of the following is changed:
//   - queues are created only in family 2               (omit the graphics family)
//   - the event is submitted on family 0 instead        (graphics queue)
//   - the stage mask is TOP_OF_PIPE / BOTTOM_OF_PIPE / HOST / COMPUTE_SHADER / ALL_COMMANDS
//   - vkCmdSetEvent2 (VK_KHR_synchronization2) is used instead of vkCmdSetEvent
//   - the vkCmdSetEvent is removed
// Also passes on llvmpipe (Mesa 25.2.8).
//
// Build: g++ -std=c++17 -O2 -o xid32_repro xid32_repro.cpp -lvulkan
// Run:   ./xid32_repro

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#define VKCHK(x)                                                                \
	do                                                                          \
	{                                                                           \
		VkResult _r = (x);                                                      \
		if(_r != VK_SUCCESS)                                                    \
		{                                                                       \
			printf("FAIL: %s returned %d at line %d\n", #x, (int)_r, __LINE__); \
			return 2;                                                           \
		}                                                                       \
	} while(0)

static const uint32_t GRAPHICS_FAMILY = 0;
static const uint32_t COMPUTE_FAMILY  = 2;

int main()
{
	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "xid32_repro";
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &appInfo;

	VkInstance instance;
	VKCHK(vkCreateInstance(&ici, nullptr, &instance));

	uint32_t ndev = 0;
	VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, nullptr));
	std::vector<VkPhysicalDevice> devices(ndev);
	VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, devices.data()));

	VkPhysicalDevice phy = devices[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phy, &props);
	printf("device: %s (driver 0x%08x, api %u.%u.%u)\n",
		props.deviceName, props.driverVersion,
		VK_VERSION_MAJOR(props.apiVersion),
		VK_VERSION_MINOR(props.apiVersion),
		VK_VERSION_PATCH(props.apiVersion));

	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phy, &nqf, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(nqf);
	vkGetPhysicalDeviceQueueFamilyProperties(phy, &nqf, qfs.data());
	if(nqf <= COMPUTE_FAMILY)
	{
		printf("device has only %u queue families, need at least %u\n", nqf, COMPUTE_FAMILY + 1);
		return 3;
	}
	printf("family %u flags 0x%02x, family %u flags 0x%02x\n",
		GRAPHICS_FAMILY, qfs[GRAPHICS_FAMILY].queueFlags,
		COMPUTE_FAMILY, qfs[COMPUTE_FAMILY].queueFlags);

	// (1) create one queue in the graphics family and one in the async-compute family
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qcis[2] = {};
	for(int i = 0; i < 2; i++)
	{
		qcis[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qcis[i].queueCount = 1;
		qcis[i].pQueuePriorities = &prio;
	}
	qcis[0].queueFamilyIndex = GRAPHICS_FAMILY;
	qcis[1].queueFamilyIndex = COMPUTE_FAMILY;

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 2;
	dci.pQueueCreateInfos = qcis;

	VkDevice dev;
	VKCHK(vkCreateDevice(phy, &dci, nullptr, &dev));

	VkQueue queue;
	vkGetDeviceQueue(dev, COMPUTE_FAMILY, 0, &queue);

	// (2) command buffer from an async-compute pool
	VkCommandPoolCreateInfo cpci = {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.queueFamilyIndex = COMPUTE_FAMILY;
	VkCommandPool cpool;
	VKCHK(vkCreateCommandPool(dev, &cpci, nullptr, &cpool));

	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = cpool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cbuf;
	VKCHK(vkAllocateCommandBuffers(dev, &cbai, &cbuf));

	VkEventCreateInfo eci = {};
	eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	VkEvent evt;
	VKCHK(vkCreateEvent(dev, &eci, nullptr, &evt));

	// (3) the only command in the buffer
	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VKCHK(vkBeginCommandBuffer(cbuf, &cbbi));
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

	printf("submitting a command buffer containing only "
		"vkCmdSetEvent(TRANSFER) on family %u...\n", COMPUTE_FAMILY);
	fflush(stdout);
	VKCHK(vkQueueSubmit(queue, 1, &si, fence));

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000);
	if(wr != VK_SUCCESS)
	{
		printf("REPRODUCED: vkWaitForFences = %d%s\n",
			(int)wr, wr == VK_ERROR_DEVICE_LOST ? " (VK_ERROR_DEVICE_LOST)" : "");
		printf("check `journalctl -k | grep Xid` for the corresponding Xid 32\n");
		return 1;
	}

	printf("NOT reproduced: fence signaled normally\n");
	return 0;
}
