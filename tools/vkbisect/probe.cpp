// Probe: compare the VK_HEADER_VERSION this TU was compiled with against the one
// baked into the dispatcher that libscopehal created. Diagnostic only.

#include "scopehal.h"
#include <cstdio>

using namespace std;

int main(int argc, char** argv)
{
	Severity console_verbosity = Severity::ERROR;
	for(int i=1; i<argc; i++)
		ParseLoggerArguments(i, argc, argv, console_verbosity);
	g_log_sinks.emplace(g_log_sinks.begin(), new ColoredSTDLogSink(console_verbosity));

	printf("probe TU  VK_HEADER_VERSION      = %d\n", VK_HEADER_VERSION);
	printf("probe TU  VULKAN_HPP_VERSION     = %d.%d.%d\n",
		VK_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
		VK_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),
		VK_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
#ifdef VK_ENABLE_BETA_EXTENSIONS
	printf("probe TU  VK_ENABLE_BETA_EXTENSIONS = defined\n");
#else
	printf("probe TU  VK_ENABLE_BETA_EXTENSIONS = NOT defined\n");
#endif
	printf("sizeof(vk::CommandBufferBeginInfo)   = %zu\n", sizeof(vk::CommandBufferBeginInfo));
	printf("sizeof(vk::SubmitInfo)               = %zu\n", sizeof(vk::SubmitInfo));
	printf("sizeof(vk::PhysicalDeviceFeatures2)  = %zu\n", sizeof(vk::PhysicalDeviceFeatures2));
	printf("sizeof(vk::WriteDescriptorSet)       = %zu\n", sizeof(vk::WriteDescriptorSet));
	printf("sizeof(vk::DeviceCreateInfo)         = %zu\n", sizeof(vk::DeviceCreateInfo));

	if(!VulkanInit(false))
	{
		printf("VulkanInit failed\n");
		return 1;
	}

	auto disp = g_vkComputeDevice->getDispatcher();
	printf("libscopehal dispatcher header version = %d\n", disp->getVkHeaderVersion());
	printf("MATCH: %s\n", disp->getVkHeaderVersion() == VK_HEADER_VERSION ? "yes" : "NO");
	return 0;
}
