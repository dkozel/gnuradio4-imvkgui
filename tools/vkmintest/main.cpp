// Minimal, scopehal-free Vulkan compute dispatch.
// Diagnostic only -- not a project deliverable.
//
// Build:
//   g++ -std=c++17 -O2 -o vkmintest main.cpp -lvulkan
//   glslangValidator -V shader.comp -o shader.spv
// Run:
//   ./vkmintest [device_index]

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

#define VKCHK(x)                                                                 \
	do                                                                           \
	{                                                                            \
		VkResult _r = (x);                                                       \
		if(_r != VK_SUCCESS)                                                     \
		{                                                                        \
			printf("FAIL: %s returned %d at line %d\n", #x, (int)_r, __LINE__);   \
			exit(1);                                                             \
		}                                                                        \
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

int main(int argc, char** argv)
{
	uint32_t devIndex = 0;
	if(argc > 1)
		devIndex = atoi(argv[1]);

	const uint32_t count = 4096;
	const VkDeviceSize bufSize = count * sizeof(float);

	//------------------------------------------------------------------
	// Instance
	//------------------------------------------------------------------
	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "vkmintest";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "none";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo icinfo = {};
	icinfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	icinfo.pApplicationInfo = &appInfo;

	VkInstance instance;
	VKCHK(vkCreateInstance(&icinfo, nullptr, &instance));

	//------------------------------------------------------------------
	// Physical device
	//------------------------------------------------------------------
	uint32_t ndev = 0;
	VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, nullptr));
	std::vector<VkPhysicalDevice> devices(ndev);
	VKCHK(vkEnumeratePhysicalDevices(instance, &ndev, devices.data()));
	printf("Found %u physical devices\n", ndev);
	for(uint32_t i = 0; i < ndev; i++)
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);
		printf("  [%u] %s (api %u.%u.%u)\n",
			i,
			props.deviceName,
			VK_VERSION_MAJOR(props.apiVersion),
			VK_VERSION_MINOR(props.apiVersion),
			VK_VERSION_PATCH(props.apiVersion));
	}
	if(devIndex >= ndev)
	{
		printf("FAIL: device index %u out of range\n", devIndex);
		return 1;
	}
	VkPhysicalDevice phy = devices[devIndex];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phy, &props);
	printf("Using device [%u]: %s\n", devIndex, props.deviceName);

	//------------------------------------------------------------------
	// Queue family with compute
	//------------------------------------------------------------------
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
	if(qfi == UINT32_MAX)
	{
		printf("FAIL: no compute queue family\n");
		return 1;
	}
	printf("Using queue family %u\n", qfi);

	//------------------------------------------------------------------
	// Logical device
	//------------------------------------------------------------------
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;

	VkDevice dev;
	VKCHK(vkCreateDevice(phy, &dci, nullptr, &dev));

	VkQueue queue;
	vkGetDeviceQueue(dev, qfi, 0, &queue);

	//------------------------------------------------------------------
	// Buffer (host visible + coherent, keeps the test small)
	//------------------------------------------------------------------
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = bufSize;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer buf;
	VKCHK(vkCreateBuffer(dev, &bci, nullptr, &buf));

	VkMemoryRequirements mreq;
	vkGetBufferMemoryRequirements(dev, buf, &mreq);

	VkPhysicalDeviceMemoryProperties mprops;
	vkGetPhysicalDeviceMemoryProperties(phy, &mprops);

	uint32_t memType = UINT32_MAX;
	VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for(uint32_t i = 0; i < mprops.memoryTypeCount; i++)
	{
		if(!(mreq.memoryTypeBits & (1 << i)))
			continue;
		if((mprops.memoryTypes[i].propertyFlags & want) == want)
		{
			memType = i;
			break;
		}
	}
	if(memType == UINT32_MAX)
	{
		printf("FAIL: no host visible coherent memory type\n");
		return 1;
	}
	printf("Using memory type %u (heap %u)\n", memType, mprops.memoryTypes[memType].heapIndex);

	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mreq.size;
	mai.memoryTypeIndex = memType;

	VkDeviceMemory mem;
	VKCHK(vkAllocateMemory(dev, &mai, nullptr, &mem));
	VKCHK(vkBindBufferMemory(dev, buf, mem, 0));

	// Fill with known values
	void* mapped = nullptr;
	VKCHK(vkMapMemory(dev, mem, 0, bufSize, 0, &mapped));
	float* fp = (float*)mapped;
	for(uint32_t i = 0; i < count; i++)
		fp[i] = (float)i;
	vkUnmapMemory(dev, mem);

	//------------------------------------------------------------------
	// Descriptor set
	//------------------------------------------------------------------
	VkDescriptorSetLayoutBinding dslb = {};
	dslb.binding = 0;
	dslb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	dslb.descriptorCount = 1;
	dslb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo dslci = {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 1;
	dslci.pBindings = &dslb;

	VkDescriptorSetLayout dsl;
	VKCHK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

	VkDescriptorPoolSize dps = {};
	dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	dps.descriptorCount = 1;

	VkDescriptorPoolCreateInfo dpci = {};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &dps;

	VkDescriptorPool dpool;
	VKCHK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));

	VkDescriptorSetAllocateInfo dsai = {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = dpool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &dsl;

	VkDescriptorSet dset;
	VKCHK(vkAllocateDescriptorSets(dev, &dsai, &dset));

	VkDescriptorBufferInfo dbi = {};
	dbi.buffer = buf;
	dbi.offset = 0;
	dbi.range = bufSize;

	VkWriteDescriptorSet wds = {};
	wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	wds.dstSet = dset;
	wds.dstBinding = 0;
	wds.descriptorCount = 1;
	wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	wds.pBufferInfo = &dbi;
	vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);

	//------------------------------------------------------------------
	// Pipeline
	//------------------------------------------------------------------
	const char* spvPath = getenv("VKMINTEST_SPV");
	if(!spvPath)
		spvPath = "shader.spv";
	auto spirv = LoadSpirv(spvPath);

	VkShaderModuleCreateInfo smci = {};
	smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = spirv.size() * 4;
	smci.pCode = spirv.data();

	VkShaderModule smod;
	VKCHK(vkCreateShaderModule(dev, &smci, nullptr, &smod));

	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.offset = 0;
	pcr.size = sizeof(uint32_t);

	VkPipelineLayoutCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &dsl;
	plci.pushConstantRangeCount = 1;
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

	VkCommandBufferBeginInfo cbbi = {};
	cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHK(vkBeginCommandBuffer(cbuf, &cbbi));

	vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, nullptr);
	vkCmdPushConstants(cbuf, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &count);
	vkCmdDispatch(cbuf, count / 64, 1, 1);

	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(
		cbuf,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT,
		0,
		1, &mb,
		0, nullptr,
		0, nullptr);

	VKCHK(vkEndCommandBuffer(cbuf));

	//------------------------------------------------------------------
	// Submit
	//------------------------------------------------------------------
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence;
	VKCHK(vkCreateFence(dev, &fci, nullptr, &fence));

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cbuf;

	printf("Submitting...\n");
	VKCHK(vkQueueSubmit(queue, 1, &si, fence));

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000);
	printf("vkWaitForFences returned %d", (int)wr);
	if(wr == VK_ERROR_DEVICE_LOST)
		printf(" (VK_ERROR_DEVICE_LOST)");
	printf("\n");
	if(wr != VK_SUCCESS)
	{
		printf("RESULT: FAIL (fence wait)\n");
		return 1;
	}

	//------------------------------------------------------------------
	// Verify
	//------------------------------------------------------------------
	VKCHK(vkMapMemory(dev, mem, 0, bufSize, 0, &mapped));
	fp = (float*)mapped;
	uint32_t bad = 0;
	for(uint32_t i = 0; i < count; i++)
	{
		float expect = (float)i * 2.0f;
		if(fp[i] != expect)
		{
			if(bad < 5)
				printf("  mismatch at %u: got %f want %f\n", i, fp[i], expect);
			bad++;
		}
	}
	vkUnmapMemory(dev, mem);

	if(bad)
	{
		printf("RESULT: FAIL (%u mismatches)\n", bad);
		return 1;
	}

	printf("RESULT: PASS (%u elements doubled correctly)\n", count);

	//------------------------------------------------------------------
	// Cleanup
	//------------------------------------------------------------------
	vkDestroyFence(dev, fence, nullptr);
	vkDestroyCommandPool(dev, cpool, nullptr);
	vkDestroyPipeline(dev, pipe, nullptr);
	vkDestroyPipelineLayout(dev, playout, nullptr);
	vkDestroyShaderModule(dev, smod, nullptr);
	vkDestroyDescriptorPool(dev, dpool, nullptr);
	vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
	vkDestroyBuffer(dev, buf, nullptr);
	vkFreeMemory(dev, mem, nullptr);
	vkDestroyDevice(dev, nullptr);
	vkDestroyInstance(instance, nullptr);
	return 0;
}
