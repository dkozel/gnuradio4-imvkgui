/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Implementation of RenderHost
 */

#include "RenderHost.h"

using namespace std;

namespace imcufosphor
{

RenderHost::RenderHost()
	: m_available(false)
	, m_texmgr(nullptr)
	, m_dpiScale(1.0f)
	, m_frameCmdBuf(nullptr)
{
}

RenderHost::~RenderHost()
{
}

void RenderHost::Publish(TextureManager* texmgr, shared_ptr<QueueHandle> queue, float dpiScale)
{
	m_texmgr = texmgr;
	m_queue = queue;
	m_dpiScale = dpiScale;

	//Released last, with release ordering, so that a scheduler thread seeing Available() go
	//true is guaranteed to see the three fields above it already written.
	m_available.store(true, memory_order_release);
}

void RenderHost::Retract()
{
	//Cleared first, so nothing starts using the resources while they are being dropped
	m_available.store(false, memory_order_release);

	m_frameCmdBuf = nullptr;
	m_texmgr = nullptr;
	m_queue.reset();
}

void RenderHost::BeginFrame(vk::raii::CommandBuffer* preRenderPassCmdBuf)
{
	m_frameCmdBuf = preRenderPassCmdBuf;
}

void RenderHost::EndFrame()
{
	//Nulled rather than left dangling: a draw() called outside a frame is a bug, and returning
	//null makes it a null check rather than a use-after-scope.
	m_frameCmdBuf = nullptr;
}

RenderHost& globalRenderHost()
{
	static RenderHost host;
	return host;
}

} // namespace imcufosphor
