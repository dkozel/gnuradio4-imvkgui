// Fully scopehal-free: submit a command buffer containing nothing but vkCmdSetEvent,
// once per queue family, and report which families survive it.
//
// Build: g++ -std=c++17 -O2 -o eventtest eventtest.cpp -lvulkan
// Diagnostic only.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#define VKCHK(x)                                                                \
	do                                                                          \
	{                                                                           \
		VkResult _r = (x);                                                      \
		if(_r != VK_SUCCESS)                                                    \
		{                                                                       \
			printf("  FAIL: %s returned %d at line %d\n", #x, (int)_r, __LINE__); \
			return 2;                                                           \
		}                                                                       \
	} while(0)

// Runs in a fresh process each time (see main) so a device loss cannot poison later tests.
static std::vector<const char*> g_exts;
static bool g_allQueues = false;
static uint32_t g_famMask = 0xffffffff;   // which families to create queues for
static int g_maxq = -1;                  // cap queues per family
static bool g_sync2 = false;             // use VK_KHR_synchronization2's vkCmdSetEvent2
static bool g_noEvent = false;
static uint32_t g_apiVer = VK_API_VERSION_1_2;

static int RunOne(uint32_t devIndex, uint32_t famIndex, uint32_t stageMask, bool useSync2)
{
	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "eventtest";
	appInfo.apiVersion = g_apiVer;

	VkInstanceCreateInfo icinfo = {};
	icinfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	icinfo.pApplicationInfo = &appInfo;

	VkInstance instance;
	VKCHK(vkCreateInstance(&icinfo, nullptr, &instance));

	uint32_t ndev = 0;
	VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, nullptr));
	std::vector<VkPhysicalDevice> devices(ndev);
	VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, devices.data()));
	VkPhysicalDevice phy = devices[devIndex];

	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phy, &nqf, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(nqf);
	vkGetPhysicalDeviceQueueFamilyProperties(phy, &nqf, qfs.data());
	if(famIndex >= nqf)
	{
		printf("  (no such family)\n");
		return 3;
	}

	std::vector<float> prios(64, 1.0f);
	std::vector<VkDeviceQueueCreateInfo> qcis;
	if(g_allQueues)
	{
		// exactly what scopehal does: every queue in every family
		for(uint32_t i = 0; i < nqf; i++)
		{
			if(!(g_famMask & (1u << i)) && i != famIndex)
				continue;
			uint32_t n = qfs[i].queueCount;
			if(g_maxq > 0 && n > (uint32_t)g_maxq)
				n = g_maxq;
			VkDeviceQueueCreateInfo q = {};
			q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			q.queueFamilyIndex = i;
			q.queueCount = n;
			q.pQueuePriorities = prios.data();
			qcis.push_back(q);
		}
	}
	else
	{
		VkDeviceQueueCreateInfo q = {};
		q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		q.queueFamilyIndex = famIndex;
		q.queueCount = 1;
		q.pQueuePriorities = prios.data();
		qcis.push_back(q);
	}

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = qcis.size();
	dci.pQueueCreateInfos = qcis.data();
	dci.enabledExtensionCount = g_exts.size();
	dci.ppEnabledExtensionNames = g_exts.data();

	VkDevice dev;
	VKCHK(vkCreateDevice(phy, &dci, nullptr, &dev));

	VkQueue queue;
	vkGetDeviceQueue(dev, famIndex, 0, &queue);

	VkCommandPoolCreateInfo cpoolci = {};
	cpoolci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpoolci.queueFamilyIndex = famIndex;
	VkCommandPool cpool;
	VKCHK(vkCreateCommandPool(dev, &cpoolci, nullptr, &cpool));

	VkCommandBufferAllocateInfo cbai = {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = cpool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cbuf;
	VKCHK(vkAllocateCommandBuffers(dev, &cbai, &cbuf));

	PFN_vkCmdSetEvent2KHR pfnSetEvent2 = nullptr;
	if(g_sync2)
	{
		pfnSetEvent2 = (PFN_vkCmdSetEvent2KHR)vkGetDeviceProcAddr(dev, "vkCmdSetEvent2KHR");
		if(!pfnSetEvent2)
		{
			printf("  (no vkCmdSetEvent2KHR)\n");
			return 3;
		}
	}

	VkEventCreateInfo eci = {};
	eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	VkEvent evt;
	VKCHK(vkCreateEvent(dev, &eci, nullptr, &evt));

	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VKCHK(vkBeginCommandBuffer(cbuf, &cbbi));
	if(g_noEvent)
	{
		// nothing
	}
	else if(g_sync2)
	{
		VkDependencyInfo dep = {};
		dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		pfnSetEvent2(cbuf, evt, &dep);
	}
	else
		vkCmdSetEvent(cbuf, evt, stageMask);
	VKCHK(vkEndCommandBuffer(cbuf));

	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence;
	VKCHK(vkCreateFence(dev, &fci, nullptr, &fence));

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cbuf;
	VKCHK(vkQueueSubmit(queue, 1, &si, fence));

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5ULL * 1000 * 1000 * 1000);
	if(wr != VK_SUCCESS)
	{
		printf("  DEVICE LOST (vkWaitForFences = %d)\n", (int)wr);
		return 1;
	}
	printf("  PASS\n");
	return 0;
}

int main(int argc, char** argv)
{
	uint32_t devIndex = 0;
	uint32_t famIndex = 0;
	uint32_t stageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	for(int i = 1; i < argc; i++)
	{
		if(!strcmp(argv[i], "--dev") && i + 1 < argc)
			devIndex = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--fam") && i + 1 < argc)
			famIndex = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--stage") && i + 1 < argc)
			stageMask = strtoul(argv[++i], nullptr, 0);
		else if(!strcmp(argv[i], "--allq"))
			g_allQueues = true;
		else if(!strcmp(argv[i], "--fammask") && i + 1 < argc)
			g_famMask = strtoul(argv[++i], nullptr, 0);
		else if(!strcmp(argv[i], "--maxq") && i + 1 < argc)
			g_maxq = atoi(argv[++i]);
		else if(!strcmp(argv[i], "--sync2"))
			g_sync2 = true;
		else if(!strcmp(argv[i], "--noevent"))
			g_noEvent = true;
		else if(!strcmp(argv[i], "--api11"))
			g_apiVer = VK_API_VERSION_1_1;
		else if(!strcmp(argv[i], "--api13"))
			g_apiVer = VK_API_VERSION_1_3;
		else if(!strcmp(argv[i], "--ext") && i + 1 < argc)
			g_exts.push_back(argv[++i]);
	}
	return RunOne(devIndex, famIndex, stageMask, false);
}
