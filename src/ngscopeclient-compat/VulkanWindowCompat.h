/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief VulkanWindow, made includable on its own

	Upstream's VulkanWindow.h has no #include directives at all - not one - and opens straight
	into a forward declaration at line 38. It names GLFWwindow, GLFWmonitor, QueueHandle and
	ten vk::raii types, and expects its includer to have read ngscopeclient.h already.

	That is the specific reason MainWindow.h and Gr4AnalyzerWindow.h could not stop including
	ngscopeclient.h even though neither wants anything else from it: dropping it would have
	broken the header below, not the file doing the dropping.

	It also holds std::shared_ptr<Texture>, so TextureCompat.h comes first.

	Deliberately scaffolding, like TextureCompat.h, and deleted at the same time.
 */
#ifndef VulkanWindowCompat_h
#define VulkanWindowCompat_h

//GLFW, ImGui, scopehal and Texture, in that order
#include "TextureCompat.h"

#include "VulkanWindow.h"

#endif
