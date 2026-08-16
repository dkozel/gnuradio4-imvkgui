// Minimal scopehal-free Vulkan compute, parameterised so scopehal's distinguishing
// choices can be switched on one at a time. Diagnostic only.
//
// Options:
//   --shader PATH    SPIR-V to run (default shader.spv, 1 SSBO)
//   --nssbo N        number of storage buffers to bind (default 1)
//   --pushdesc       use VK_KHR_push_descriptor instead of a normal descriptor set
//   --pushconst N    push constant size in bytes (default 4)
//   --devlocal       put buffers in DEVICE_LOCAL memory + staging copy (like AcceleratorBuffer)
//   --dev N          physical device index (default 0)
//   --wg N           workgroup count (default 64)
//
// Build: g++ -std=c++17 -O2 -o vkmintest2 main2.cpp -lvulkan

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

#define VKCHK(x)                                                               \
	do                                                                         \
	{                                                                          \
		VkResult _r = (x);                                                     \
		if(_r != VK_SUCCESS)                                                   \
		{                                                                      \
			printf("FAIL: %s returned %d at line %d\n", #x, (int)_r, __LINE__); \
			exit(1);                                                           \
		}                                                                      \
	} while(0)

static std::vector<uint32_t> LoadSpirv(const char* path)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if(!f)
	{
		printf("FAIL: cannot open %s\n", path);
		exit(1);
	}
	size_t len = f.tellg();
	f.seekg(0);
	std::vector<uint32_t> out(len / 4);
	f.read((char*)out.data(), len);
	return out;
}

static uint32_t FindMemType(
	VkPhysicalDeviceMemoryProperties const& mprops,
	uint32_t bits,
	VkMemoryPropertyFlags want)
{
	for(uint32_t i = 0; i < mprops.memoryTypeCount; i++)
	{
		if(!(bits & (1 << i)))
			continue;
		if((mprops.memoryTypes[i].propertyFlags & want) == want)
			return i;
	}
	return UINT32_MAX;
}

int main(int argc, char** argv)
{
	const char* shaderPath = "shader.spv";
	uint32_t nssbo = 1;
	bool pushdesc = false;
	uint32_t pushConstSize = 4;
	bool devlocal = false;
	bool useEvent = false;
	uint32_t devIndex = 0;
	uint32_t wgcount = 64;

	for(int i = 1; i < argc; i++)
	{
		std::string a(argv[i]);
		if(a == "--shader" && i + 1 < argc)
			shaderPath = argv[++i];
		else if(a == "--nssbo" && i + 1 < argc)
			nssbo = atoi(argv[++i]);
		else if(a == "--pushdesc")
			pushdesc = true;
		else if(a == "--pushconst" && i + 1 < argc)
			pushConstSize = atoi(argv[++i]);
		else if(a == "--event")
			useEvent = true;
		else if(a == "--devlocal")
			devlocal = true;
		else if(a == "--dev" && i + 1 < argc)
			devIndex = atoi(argv[++i]);
		else if(a == "--wg" && i + 1 < argc)
			wgcount = atoi(argv[++i]);
		else
		{
			printf("unknown arg %s\n", argv[i]);
			return 1;
		}
	}

	printf("config: shader=%s nssbo=%u pushdesc=%d pushconst=%u devlocal=%d wg=%u event=%d\n",
		shaderPath, nssbo, (int)pushdesc, pushConstSize, (int)devlocal, wgcount, (int)useEvent);

	const uint32_t count = wgcount * 64;
	const VkDeviceSize bufSize = count * sizeof(float);

	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "vkmintest2";
	appInfo.apiVersion = VK_API_VERSION_1_2;

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
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phy, &props);
	printf("device: %s\n", props.deviceName);

	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phy, &nqf, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(nqf);
	vkGetPhysicalDeviceQueueFamilyProperties(phy, &nqf, qfs.data());
	uint32_t qfi = UINT32_MAX;
	for(uint32_t i = 0; i < nqf; i++)
	{
		if(qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			qfi = i;
			break;
		}
	}
	printf("queue family: %u\n", qfi);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	std::vector<const char*> devexts;
	if(pushdesc)
		devexts.push_back("VK_KHR_push_descriptor");

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = devexts.size();
	dci.ppEnabledExtensionNames = devexts.data();

	VkDevice dev;
	VKCHK(vkCreateDevice(phy, &dci, nullptr, &dev));

	VkQueue queue;
	vkGetDeviceQueue(dev, qfi, 0, &queue);

	PFN_vkCmdPushDescriptorSetKHR pfnPushDesc = nullptr;
	if(pushdesc)
	{
		pfnPushDesc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
		if(!pfnPushDesc)
		{
			printf("FAIL: no vkCmdPushDescriptorSetKHR\n");
			return 1;
		}
	}

	VkPhysicalDeviceMemoryProperties mprops;
	vkGetPhysicalDeviceMemoryProperties(phy, &mprops);

	//------------------------------------------------------------------
	// Buffers
	//------------------------------------------------------------------
	std::vector<VkBuffer> bufs(nssbo);
	std::vector<VkDeviceMemory> mems(nssbo);
	std::vector<VkBuffer> stagingBufs(nssbo);
	std::vector<VkDeviceMemory> stagingMems(nssbo);

	VkMemoryPropertyFlags wantMain = devlocal
		? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		: (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	for(uint32_t b = 0; b < nssbo; b++)
	{
		VkBufferCreateInfo bci = {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = bufSize;
		bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VKCHK(vkCreateBuffer(dev, &bci, nullptr, &bufs[b]));

		VkMemoryRequirements mreq;
		vkGetBufferMemoryRequirements(dev, bufs[b], &mreq);
		uint32_t mt = FindMemType(mprops, mreq.memoryTypeBits, wantMain);
		if(mt == UINT32_MAX)
		{
			printf("FAIL: no memory type for buffer %u\n", b);
			return 1;
		}
		if(b == 0)
			printf("main buffer memory type %u (heap %u)\n", mt, mprops.memoryTypes[mt].heapIndex);

		VkMemoryAllocateInfo mai = {};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = mreq.size;
		mai.memoryTypeIndex = mt;
		VKCHK(vkAllocateMemory(dev, &mai, nullptr, &mems[b]));
		VKCHK(vkBindBufferMemory(dev, bufs[b], mems[b], 0));

		if(devlocal)
		{
			VkBufferCreateInfo sbci = bci;
			VKCHK(vkCreateBuffer(dev, &sbci, nullptr, &stagingBufs[b]));
			VkMemoryRequirements smreq;
			vkGetBufferMemoryRequirements(dev, stagingBufs[b], &smreq);
			uint32_t smt = FindMemType(mprops, smreq.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VkMemoryAllocateInfo smai = {};
			smai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			smai.allocationSize = smreq.size;
			smai.memoryTypeIndex = smt;
			VKCHK(vkAllocateMemory(dev, &smai, nullptr, &stagingMems[b]));
			VKCHK(vkBindBufferMemory(dev, stagingBufs[b], stagingMems[b], 0));
		}
	}

	// Fill inputs
	for(uint32_t b = 0; b < nssbo; b++)
	{
		VkDeviceMemory target = devlocal ? stagingMems[b] : mems[b];
		void* mapped = nullptr;
		VKCHK(vkMapMemory(dev, target, 0, bufSize, 0, &mapped));
		float* fp = (float*)mapped;
		for(uint32_t i = 0; i < count; i++)
			fp[i] = (float)i * (b + 1);
		vkUnmapMemory(dev, target);
	}

	//------------------------------------------------------------------
	// Descriptors
	//------------------------------------------------------------------
	std::vector<VkDescriptorSetLayoutBinding> dslbs(nssbo);
	for(uint32_t b = 0; b < nssbo; b++)
	{
		dslbs[b] = {};
		dslbs[b].binding = b;
		dslbs[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		dslbs[b].descriptorCount = 1;
		dslbs[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = nssbo;
	dslci.pBindings = dslbs.data();
	if(pushdesc)
		dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;

	VkDescriptorSetLayout dsl;
	VKCHK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

	std::vector<VkDescriptorBufferInfo> dbis(nssbo);
	for(uint32_t b = 0; b < nssbo; b++)
	{
		dbis[b] = {};
		dbis[b].buffer = bufs[b];
		dbis[b].offset = 0;
		dbis[b].range = bufSize;
	}

	std::vector<VkWriteDescriptorSet> wdss(nssbo);
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet dset = VK_NULL_HANDLE;

	if(!pushdesc)
	{
		VkDescriptorPoolSize dps = {};
		dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		dps.descriptorCount = nssbo;

		VkDescriptorPoolCreateInfo dpci = {};
		dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dpci.maxSets = 1;
		dpci.poolSizeCount = 1;
		dpci.pPoolSizes = &dps;
		VKCHK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));

		VkDescriptorSetAllocateInfo dsai = {};
		dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dsai.descriptorPool = dpool;
		dsai.descriptorSetCount = 1;
		dsai.pSetLayouts = &dsl;
		VKCHK(vkAllocateDescriptorSets(dev, &dsai, &dset));
	}

	for(uint32_t b = 0; b < nssbo; b++)
	{
		wdss[b] = {};
		wdss[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wdss[b].dstSet = pushdesc ? VK_NULL_HANDLE : dset;
		wdss[b].dstBinding = b;
		wdss[b].descriptorCount = 1;
		wdss[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		wdss[b].pBufferInfo = &dbis[b];
	}
	if(!pushdesc)
		vkUpdateDescriptorSets(dev, nssbo, wdss.data(), 0, nullptr);

	//------------------------------------------------------------------
	// Pipeline
	//------------------------------------------------------------------
	auto spirv = LoadSpirv(shaderPath);
	printf("spirv: %zu words\n", spirv.size());

	VkShaderModuleCreateInfo smci = {};
	smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = spirv.size() * 4;
	smci.pCode = spirv.data();

	VkShaderModule smod;
	VKCHK(vkCreateShaderModule(dev, &smci, nullptr, &smod));

	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.offset = 0;
	pcr.size = pushConstSize;

	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsl;
	plci.pushConstantRangeCount = pushConstSize ? 1 : 0;
	plci.pPushConstantRanges = &pcr;

	VkPipelineLayout playout;
	VKCHK(vkCreatePipelineLayout(dev, &plci, nullptr, &playout));

	VkComputePipelineCreateInfo cpci = {};
	cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = smod;
	cpci.stage.pName = "main";
	cpci.layout = playout;

	VkPipeline pipe;
	VKCHK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));

	//------------------------------------------------------------------
	// Command buffer
	//------------------------------------------------------------------
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

	std::vector<VkEvent> events(nssbo, VK_NULL_HANDLE);
	if(useEvent)
	{
		for(uint32_t b = 0; b < nssbo; b++)
		{
			VkEventCreateInfo eci = {};
			eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
			VKCHK(vkCreateEvent(dev, &eci, nullptr, &events[b]));
		}
		printf("created %u events\n", nssbo);
	}

	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHK(vkBeginCommandBuffer(cbuf, &cbbi));

	if(devlocal)
	{
		for(uint32_t b = 0; b < nssbo; b++)
		{
			VkBufferCopy region = {};
			region.size = bufSize;
			vkCmdCopyBuffer(cbuf, stagingBufs[b], bufs[b], 1, &region);
			if(useEvent)
				vkCmdSetEvent(cbuf, events[b], VK_PIPELINE_STAGE_TRANSFER_BIT);
		}
		VkMemoryBarrier mb = {};
		mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	}

	if(useEvent && !devlocal)
	{
		for(uint32_t b = 0; b < nssbo; b++)
			vkCmdSetEvent(cbuf, events[b], VK_PIPELINE_STAGE_TRANSFER_BIT);
	}

	vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

	if(pushConstSize)
	{
		std::vector<uint8_t> pc(pushConstSize, 0);
		uint32_t c = count;
		memcpy(pc.data(), &c, sizeof(uint32_t) < pushConstSize ? sizeof(uint32_t) : pushConstSize);
		vkCmdPushConstants(cbuf, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstSize, pc.data());
	}

	if(pushdesc)
		pfnPushDesc(cbuf, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, nssbo, wdss.data());
	else
		vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, nullptr);

	vkCmdDispatch(cbuf, wgcount, 1, 1);

	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &mb, 0, nullptr, 0, nullptr);

	if(devlocal)
	{
		for(uint32_t b = 0; b < nssbo; b++)
		{
			VkBufferCopy region = {};
			region.size = bufSize;
			vkCmdCopyBuffer(cbuf, bufs[b], stagingBufs[b], 1, &region);
		}
	}

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
	VKCHK(vkQueueSubmit(queue, 1, &si, fence));

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000);
	if(wr != VK_SUCCESS)
	{
		printf("RESULT: FAIL - vkWaitForFences returned %d%s\n",
			(int)wr, wr == VK_ERROR_DEVICE_LOST ? " (VK_ERROR_DEVICE_LOST)" : "");
		return 1;
	}

	// Read back last buffer
	{
		VkDeviceMemory target = devlocal ? stagingMems[nssbo - 1] : mems[nssbo - 1];
		void* mapped = nullptr;
		VKCHK(vkMapMemory(dev, target, 0, bufSize, 0, &mapped));
		float* fp = (float*)mapped;
		printf("output[0..3] = %f %f %f %f\n", fp[0], fp[1], fp[2], fp[3]);
		vkUnmapMemory(dev, target);
	}

	printf("RESULT: PASS\n");
	return 0;
}
