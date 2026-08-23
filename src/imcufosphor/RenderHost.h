/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of RenderHost
 */
#ifndef RenderHost_h
#define RenderHost_h

#include "TextureManager.h"

#include <atomic>
#include <memory>

namespace imcufosphor
{

/**
	@brief How a display object finds the window it is supposed to draw into

	A GNU Radio block is constructed by emplaceBlock() with a property_map, from a thread and at
	a time it does not choose. It cannot be handed a TextureManager, a Vulkan queue or a command
	buffer through its constructor, because the graph that builds it knows about none of those.
	So the host publishes them here and the block picks them up on its first draw().

	@par This is global state, chosen with open eyes

	scopehal is already built on g_vkComputeDevice, g_vkQueueManager and g_log_sinks, so a
	fourth process-global costs nothing structurally new, and it is the same shape as gnuradio4's
	own globalDataSinkRegistry(). The alternative - threading a context pointer through
	property_map settings as an integer - is worse in every way that matters: unchecked, untyped,
	and invisible to anything reading the flowgraph.

	@par Availability is the whole point

	A headless flowgraph never creates a window, so nothing is ever published and Available()
	stays false. Blocks check it before allocating anything, which is what lets the same graph
	run with no GPU at all - the sink degenerates to consuming and dropping. Being unavailable
	is a supported state, not an error.

	@par Threading

	Available() is atomic and is the only member safe to call from a scheduler thread; it exists
	so a block can report a headless run at start(). Everything else is render-thread-only, and
	FrameComputeCommandBuffer() is valid only between BeginFrame() and EndFrame().
 */
class RenderHost
{
public:
	RenderHost();
	~RenderHost();

	//not copyable or assignable
	RenderHost(const RenderHost&) =delete;
	RenderHost& operator=(const RenderHost&) =delete;

	/**
		@brief True once a host has published a window

		Safe to call from any thread. False means headless: draw nothing, allocate nothing.
	 */
	bool Available() const
	{ return m_available.load(std::memory_order_acquire); }

	///@brief Colour ramps and icons. Render thread only, and only while Available().
	TextureManager* Textures() const
	{ return m_texmgr; }

	///@brief The queue the window renders on. Render thread only.
	std::shared_ptr<QueueHandle> RenderQueue() const
	{ return m_queue; }

	///@brief Content scale of the window, for sizing text and gutters
	float DpiScale() const
	{ return m_dpiScale; }

	/**
		@brief A scratch command buffer for compute work during a frame

		@par Ownership of the recording is the caller's

		The caller does begin(), records, does end(), and submits through RenderQueue(). This
		only supplies the buffer, so that a dozen display blocks in one flowgraph do not each
		allocate a pool.

		@par Why the caller submits rather than the host

		draw() is invoked from inside the ImGui frame, which is inside the window's render pass
		recording. A compute dispatch cannot be recorded into a command buffer that has an
		active render pass, but a *separate* buffer submitted at that moment is fine and
		executes before the render pass buffer is submitted - which is exactly the ordering the
		tone map needs, because the ImGui draw call issued immediately afterwards samples the
		texture it just wrote. Deferring the submit to the end of the frame would put it after
		the pass that reads it.

		Callers submit one at a time and block, so sharing one buffer between blocks is safe.

		Valid only between BeginFrame() and EndFrame(); null outside that, which a caller must
		check rather than assume.
	 */
	vk::raii::CommandBuffer* FrameComputeCommandBuffer() const
	{ return m_frameCmdBuf; }

	/**
		@brief Publishes a window's resources

		Called once by the host after the window exists and before the flowgraph starts.
		Neither pointer is owned; Retract() must be called before either is destroyed.
	 */
	void Publish(TextureManager* texmgr, std::shared_ptr<QueueHandle> queue, float dpiScale);

	/**
		@brief Withdraws the resources

		Must be called before the window is destroyed and after the scheduler has been joined,
		so that no draw() can be in flight against a half-torn-down window.
	 */
	void Retract();

	///@brief Opens a frame, publishing the command buffer that draw() should record into
	void BeginFrame(vk::raii::CommandBuffer* preRenderPassCmdBuf);

	///@brief Closes a frame. The command buffer is invalid from here until the next BeginFrame().
	void EndFrame();

protected:
	std::atomic<bool> m_available;

	TextureManager* m_texmgr;
	std::shared_ptr<QueueHandle> m_queue;
	float m_dpiScale;
	vk::raii::CommandBuffer* m_frameCmdBuf;
};

/**
	@brief The process-wide render host

	Deliberately a function rather than a global object, so construction is ordered by first use
	and cannot race the static initialization of anything else.
 */
RenderHost& globalRenderHost();

} // namespace imcufosphor

#endif
