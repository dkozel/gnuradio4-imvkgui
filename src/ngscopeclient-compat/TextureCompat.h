/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief TextureManager, made includable on its own

	Upstream's TextureManager.h names GLFWimage (:114), ImTextureID (:70), QueueHandle,
	LogFatal and a dozen vk::raii types, and includes none of them. It compiles only because
	every file that uses it happens to include ngscopeclient.h first. That is not a property
	of TextureManager.h, it is a property of the include order at each call site, and it is
	why our display headers were reaching for a 1200-line umbrella to get at a texture class.

	This supplies exactly what that header leans on, then the header. Include this instead of
	TextureManager.h.

	Deliberately scaffolding. When TextureManager is vendored it becomes self-contained and
	this file is deleted.
 */
#ifndef TextureCompat_h
#define TextureCompat_h

//GLFWimage, for TextureManager::LoadPNGToGLFWImage(). GLFW_INCLUDE_NONE keeps GLFW from
//dragging in a GL header; GLFW_INCLUDE_VULKAN is what gives the ImGui Vulkan backend its
//vulkan.h. Both spellings match ngscopeclient.h:34-36, so a translation unit that includes
//both still sees one consistent GLFW.
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

//ImTextureID
#include "ImGuiCompat.h"

//QueueHandle, LogFatal, and the vk::raii types in Texture's interface
#include <scopehal/scopehal.h>

#include "TextureManager.h"

#endif
