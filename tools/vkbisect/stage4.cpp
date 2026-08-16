// Stage 4: use the VkDevice that scopehal's VulkanInit() created, but perform the whole
// compute dispatch by hand with raw Vulkan -- our own buffers, our own descriptor set,
// our own command pool and queue. No ComputePipeline, no AcceleratorBuffer, no VkFFT.
//
// If this faults, the problem is in how scopehal CREATES the device.
// If this passes, the problem is in scopehal's dispatch/buffer machinery.
//
// Diagnostic only.

#include "scopehal.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
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

static vector<uint32_t> LoadSpirv(const char* path)
{
	ifstream f(path, ios::binary | ios::ate);
	if(!f)
	{
		printf("cannot open %s\n", path);
		exit(1);
	}
	size_t len = f.tellg();
	f.seekg(0);
	vector<uint32_t> out(len / 4);
	f.read((char*)out.data(), len);
	return out;
}

int main(int argc, char** argv)
{
	Severity console_verbosity = Severity::NOTICE;
	const char* shaderPath = "shaders/AddFilter.spv";
	for(int i = 1; i < argc; i++)
	{
		if(ParseLoggerArguments(i, argc, argv, console_verbosity))
			continue;
		shaderPath = argv[i];
	}
	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	if(!VulkanInit(false))
	{
		printf("VulkanInit failed\n");
		return 1;
	}

	//Everything below is raw Vulkan on scopehal's device.
	VkDevice dev = **g_vkComputeDevice;
	VkPhysicalDevice phy = *(*g_vkComputePhysicalDevice);

	auto q = g_vkQueueManager->GetComputeQueue("stage4");
	uint32_t qfi = q->GetQueue()->m_family;
	VkQueue queue = **(q->GetQueue()->GetQueue());
	printf("using scopehal device %p, queue family %u\n", (void*)dev, qfi);

	const uint32_t nssbo = 3;
	const uint32_t wgcount = 64;
	const uint32_t count = wgcount * 64;
	const VkDeviceSize bufSize = count * sizeof(float);

	VkPhysicalDeviceMemoryProperties mprops;
	vkGetPhysicalDeviceMemoryProperties(phy, &mprops);

	VkBuffer bufs[nssbo];
	VkDeviceMemory mems[nssbo];
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
		uint32_t mt = UINT32_MAX;
		VkMemoryPropertyFlags want =
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for(uint32_t i = 0; i < mprops.memoryTypeCount; i++)
		{
			if(!(mreq.memoryTypeBits & (1 << i)))
				continue;
			if((mprops.memoryTypes[i].propertyFlags & want) == want)
			{
				mt = i;
				break;
			}
		}
		VkMemoryAllocateInfo mai = {};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = mreq.size;
		mai.memoryTypeIndex = mt;
		VKCHK(vkAllocateMemory(dev, &mai, nullptr, &mems[b]));
		VKCHK(vkBindBufferMemory(dev, bufs[b], mems[b], 0));

		void* mapped = nullptr;
		VKCHK(vkMapMemory(dev, mems[b], 0, bufSize, 0, &mapped));
		float* fp = (float*)mapped;
		for(uint32_t i = 0; i < count; i++)
			fp[i] = (float)i * (b + 1);
		vkUnmapMemory(dev, mems[b]);
	}

	VkDescriptorSetLayoutBinding dslbs[nssbo];
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
	dslci.pBindings = dslbs;
	VkDescriptorSetLayout dsl;
	VKCHK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

	VkDescriptorPoolSize dps = {};
	dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	dps.descriptorCount = nssbo;
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

	VkDescriptorBufferInfo dbis[nssbo];
	VkWriteDescriptorSet wdss[nssbo];
	for(uint32_t b = 0; b < nssbo; b++)
	{
		dbis[b] = {};
		dbis[b].buffer = bufs[b];
		dbis[b].offset = 0;
		dbis[b].range = bufSize;
		wdss[b] = {};
		wdss[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wdss[b].dstSet = dset;
		wdss[b].dstBinding = b;
		wdss[b].descriptorCount = 1;
		wdss[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		wdss[b].pBufferInfo = &dbis[b];
	}
	vkUpdateDescriptorSets(dev, nssbo, wdss, 0, nullptr);

	auto spirv = LoadSpirv(shaderPath);
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
	uint32_t sz = count;
	vkCmdPushConstants(cbuf, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &sz);
	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, nullptr);
	vkCmdDispatch(cbuf, wgcount, 1, 1);
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &mb, 0, nullptr, 0, nullptr);
	VKCHK(vkEndCommandBuffer(cbuf));

	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence;
	VKCHK(vkCreateFence(dev, &fci, nullptr, &fence));

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cbuf;

	printf("submitting raw dispatch on scopehal's device...\n");
	fflush(stdout);
	VKCHK(vkQueueSubmit(queue, 1, &si, fence));

	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000);
	if(wr != VK_SUCCESS)
	{
		printf("STAGE 4 FAIL: vkWaitForFences returned %d%s\n",
			(int)wr, wr == VK_ERROR_DEVICE_LOST ? " (VK_ERROR_DEVICE_LOST)" : "");
		return 1;
	}

	void* mapped = nullptr;
	VKCHK(vkMapMemory(dev, mems[2], 0, bufSize, 0, &mapped));
	float* fp = (float*)mapped;
	printf("output[0..3] = %f %f %f %f\n", fp[0], fp[1], fp[2], fp[3]);
	bool ok = true;
	for(uint32_t i = 0; i < count; i++)
	{
		if(fabs(fp[i] - (float)i * 3) > 1e-3)
		{
			ok = false;
			break;
		}
	}
	vkUnmapMemory(dev, mems[2]);

	printf("STAGE 4 %s\n", ok ? "PASS" : "FAIL (bad data)");
	return ok ? 0 : 1;
}
