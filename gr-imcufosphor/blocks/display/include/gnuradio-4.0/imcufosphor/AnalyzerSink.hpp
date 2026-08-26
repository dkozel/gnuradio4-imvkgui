/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of AnalyzerSink
 */
#ifndef GR_IMCUFOSPHOR_ANALYZER_SINK_HPP
#define GR_IMCUFOSPHOR_ANALYZER_SINK_HPP

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <gnuradio-4.0/imcufosphor/detail/AnnotationsFromTags.hpp>
#include <gnuradio-4.0/imcufosphor/detail/IqRing.hpp>
#include <gnuradio-4.0/imcufosphor/detail/MetaFromTags.hpp>
#include <gnuradio-4.0/imcufosphor/detail/StreamStatus.hpp>

#include "AnalyzerPane.h"
#include "RenderHost.h"
#include "SpectrumEngine.h"

namespace gr::imcufosphor {

using namespace gr;

GR_REGISTER_BLOCK("gr::imcufosphor::AnalyzerSink", gr::imcufosphor::AnalyzerSink, ([T]),
	[ std::complex<float>, std::complex<std::int16_t> ])

/**
	@brief Spectrum and waterfall display, GPU computed and GPU rendered

	Wraps the imcufosphor analyzer pane as a GNU Radio sink. The transform, the density
	histogram, the waterfall and the tone mapping all run on the GPU exactly as they do in the
	standalone application - this block supplies samples to that pipeline and drives its frame,
	it does not reimplement any of it.

	@par Why the FFT is inside the block

	ComplexFFTFilter performs every transform in a block as one batched VkFFT dispatch, which
	DESIGN.md section 13 measures as a 14.8x throughput improvement over dispatching per
	transform, and SpectrumDensity's input contract is a whole block of spectra back to back so
	that its accumulate and fold passes run at different cadences. A pre-transformed input port
	would give up both. The cost, stated plainly: this sink is heavyweight, owning a VkFFT plan
	and several compute pipelines, and two of them in one graph means two FFTs.

	@par Threading

	processBulk() runs on a scheduler thread and does nothing but scan tags and copy samples
	into a lock-free ring. draw() runs on the application's render thread and does everything
	else: the Vulkan allocation, the graph step, the tone map and the ImGui calls. Nothing
	Vulkan is touched from the scheduler thread, including in settingsChanged(), which only
	raises a flag for draw() to act on.

	@par Backpressure: none, by design

	processBulk() always consumes its whole span and drops whatever will not fit in the ring,
	counting it. A display must not be able to stall a flowgraph, and a headless graph - one
	where no application ever publishes a RenderHost, so draw() is never called - runs at full
	speed with this block as a bit bucket.

	@par Centre frequency

	Taken from the @c frequency tag (gr::tag::FREQUENCY), falling back to
	@c trigger_meta_info["core:frequency"] for SigMF interop, then to the @c center_frequency
	setting. See detail/MetaFromTags.hpp.
 */
template<typename T>
requires std::same_as<T, std::complex<float>> || std::same_as<T, std::complex<std::int16_t>>
struct AnalyzerSink : Block<AnalyzerSink<T>, Drawable<UICategory::Content, "ImGui">>
{
	using Description = Doc<R""(@brief Vulkan spectrum + waterfall analyzer display.

Consumes complex IQ and renders a fosphor-style density spectrum stacked over a waterfall on a
shared frequency axis. Centre frequency comes from the 'frequency' tag, falling back to
trigger_meta_info["core:frequency"] and then to the center_frequency setting; sample rate from
the 'sample_rate' tag unless ignore_tag_sample_rate is set.

Rendering requires a host application to have published an imcufosphor RenderHost. Without one
the block runs headless: it consumes and drops samples without allocating anything on the GPU.)"">;

	template<typename U, gr::meta::fixed_string TDescription = "", typename... Arguments>
	using A = Annotated<U, TDescription, Arguments...>;

	PortIn<T> in;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Settings

	A<gr::Size_t, "fft_size", Visible, Doc<"Transform length, which sets the resolution bandwidth">>
		fft_size = 8192U;

	A<gr::Size_t, "block_size", Visible,
		Doc<"IQ samples per GPU submit. Cut into as many transforms as fit; larger amortizes the fence round trip">>
		block_size = 1048576U;

	A<gr::Size_t, "group_size", Visible, Doc<"Spectra folded into each waterfall row">>
		group_size = 1U;

	A<float, "sample_rate", Visible, Unit<"Hz">, Doc<"Fallback sample rate, used until a tag supplies one">>
		sample_rate = 1.0f;

	A<double, "center_frequency", Visible, Unit<"Hz">,
		Doc<"Fallback centre frequency. double because float cannot hold a GHz carrier to sub-Hz">>
		center_frequency = 0.0;

	A<bool, "ignore_tag_sample_rate", Doc<"Use the sample_rate setting even when the stream tags one">>
		ignore_tag_sample_rate = false;

	A<float, "sample_scale", Doc<"Raw LSB to volts for integer input; 1/32767 makes full scale read 0 dBFS">>
		sample_scale = 1.0f / 32767.0f;

	A<std::string, "window", Visible,
		Doc<"blackman-harris | hamming | hann | rectangular | blackman | cosine-sum">>
		window = "blackman-harris";

	A<float, "db_min", Visible, Unit<"dBm">, Doc<"Bottom of the colour scale">>
		db_min = -100.0f;

	A<float, "db_max", Visible, Unit<"dBm">, Doc<"Top of the colour scale">>
		db_max = -20.0f;

	A<std::string, "color_map", Doc<"Colour ramp name, resolved by the host's TextureManager">>
		color_map = "eye-gradient-viridis";

	A<float, "spectrum_fraction", Doc<"Fraction of the pane height given to the spectrum">>
		spectrum_fraction = 0.35f;

	A<std::uint64_t, "timeout_ms", Unit<"ms">,
		Doc<"Minimum interval between draws. 0 draws every frame">>
		timeout_ms = 33ULL;

	A<gr::Size_t, "buffer_depth", Doc<"Handoff ring capacity in samples. 0 means four blocks">>
		buffer_depth = 0U;

	/**
		@brief Wall-clock milliseconds per frame spent pushing blocks through the GPU

		The single most important number for throughput. Each frame drains as many whole blocks
		as fit in this budget rather than exactly one, because one block per frame caps the
		display at block_size times the frame rate no matter how fast the pipeline is - at a
		64k block and 30 fps that is 2 MS/s against a pipeline measured at 490 MS/s, i.e. 99.6%
		of the stream discarded for no reason. MainWindow has always done this; the same
		comment lives at MainWindow.cpp:105-108.

		Bounded rather than unbounded so that a fast source cannot hold the render thread
		inside the drain loop and stall presentation.
	 */
	A<float, "step_budget_ms", Visible, Unit<"ms">,
		Doc<"Wall clock per frame spent stepping the GPU pipeline">>
		step_budget_ms = 10.0f;

	/**
		@brief Consume only what fits, throttling the upstream graph instead of dropping

		Off by default, which is right for a live radio: a display must never be able to stall
		an SDR, because the samples it fails to take are gone from the air and an overflow is
		worse than a dropped frame.

		Wrong for a file, though, and that is the common case. A recording has no realtime
		constraint, so dropping from it means analysing a fraction of the capture and quietly
		showing a spectrum of that fraction. With this on, a file source is paced by the
		display and every sample is eventually transformed.
	 */
	A<bool, "backpressure", Visible,
		Doc<"Throttle the upstream graph rather than dropping. Use for files, not for radios.">>
		backpressure = false;

	/**
		@brief Draw SigMF annotations published by the source as stream tags

		On by default. A stream that carries no annotation tags produces an empty set and the
		overlay is never attached, so this costs a string compare per tag on a graph that has
		none - and tags are per event, not per sample.
	 */
	A<bool, "annotations", Visible,
		Doc<"Draw SigMF annotation tags over the spectrum and waterfall">>
		annotations = true;

	GR_MAKE_REFLECTABLE(AnalyzerSink, in, fft_size, block_size, group_size, sample_rate,
		center_frequency, ignore_tag_sample_rate, sample_scale, window, db_min, db_max,
		color_map, spectrum_fraction, timeout_ms, buffer_depth, step_budget_ms, backpressure,
		annotations);

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// State
	//
	// Which thread owns what. Getting this wrong is the failure mode this block is most
	// exposed to, so it is written down rather than inferred:
	//
	//   member                              written by            read by
	//   ---------------------------------------------------------------------------------
	//   _ring (writer side)                 scheduler             -
	//   _ring (reader side)                 render                -
	//   _ring counters                      scheduler             render (display only)
	//   _metaRate, _metaCenter, _metaDirty  scheduler             render (top of draw)
	//   _reconfigure                        scheduler + render    render
	//   Annotated<> settings                scheduler (settings)  render (snapshot per frame)
	//   _annotationStaging                  scheduler             render (drained under lock)
	//   _annotationSet, _annotationScratch,
	//   _annotationsAttached                render only           render only
	//   _engine, _pane, _initialised,
	//   _lastDraw                           render only           render only
	//
	// start() sizes _ring, but runs before either thread is processing.

	///@brief The scheduler-to-renderer handoff. See detail/IqRing.hpp.
	detail::IqRing<T> _ring;

	/**
		@brief Ceiling on annotations waiting to be picked up

		The writer runs whether or not anything drains it: a headless flowgraph never calls
		draw() at all. Without a bound that is an unbounded leak on any graph that has a
		display block but no window.
	 */
	static constexpr std::size_t g_maxStagedAnnotations = 65536UZ;

	/**
		@brief Size at which the live set is pruned back to what the waterfall can still show

		Not a hard limit on what is drawn - PruneAnnotations() keeps everything still on screen
		however many that is - but the point at which it becomes worth walking the set to find
		out. See PruneAnnotations().
	 */
	static constexpr std::size_t g_maxLiveAnnotations = 16384UZ;

	/**
		@brief Annotations converted on the scheduler thread, waiting to be picked up

		A mutex here rather than another lock-free ring, and the difference from _ring is the
		point: that one carries every sample and a lock held across a megasample copy shows up
		directly as scheduler jitter. This carries one small struct per annotation *event*, so
		the lock is taken a few times a second on a dense recording and not at all on a live
		radio. Paying for a second SPSC queue to avoid it would be optimising the wrong thing.

		Held only long enough to swap the vector with _annotationScratch, never across the
		conversion or the sort.
	 */
	std::mutex _annotationMutex;
	std::vector<::Annotation> _annotationStaging;

	/**
		@brief Annotations the overlay draws, in SpectrumEngine's stream coordinates

		Render thread only. The pane holds a pointer to this, so it must outlive _pane - which
		it does by being declared here and destroyed after it.
	 */
	::AnnotationSet _annotationSet;

	///@brief Swapped with _annotationStaging so the drain allocates nothing in steady state
	std::vector<::Annotation> _annotationScratch;

	///@brief True once the set has been handed to the pane, which happens on the first arrival
	bool _annotationsAttached = false;

	///@brief Annotations refused because the staging buffer was full. Display only.
	std::atomic<std::uint64_t> _annotationsDropped{0};

	///@brief Latest tagged sample rate, or 0 if the stream has never tagged one
	std::atomic<double> _metaRate{0.0};

	///@brief Latest tagged centre frequency
	std::atomic<double> _metaCenter{0.0};
	std::atomic<bool> _metaDirty{false};

	/**
		@brief Set when something draw() has to apply has changed

		settingsChanged() runs on a scheduler thread and must not touch Vulkan, so everything
		that reallocates is deferred to the top of the next draw().
	 */
	std::atomic<bool> _reconfigure{true};

	///@brief What the stream has reported about itself. See detail/StreamStatus.hpp.
	detail::StreamStatus _status;

	//Render thread only, all of it. Created on the first draw() rather than in the constructor
	//or start(): emplaceBlock may run before VulkanInit, and start() runs on a scheduler thread.
	std::unique_ptr<SpectrumEngine> _engine;
	std::unique_ptr<AnalyzerPane> _pane;
	bool _initialised = false;
	std::chrono::steady_clock::time_point _lastDraw{};

	//Per-frame instrumentation. Render thread only; read by the host for its status line.
	//Without these it is impossible to tell a slow drain from a slow present, which are
	//opposite problems with opposite fixes.
	std::size_t _lastDrainSteps = 0;
	double _lastDrainMs = 0;
	double _lastToneMapMs = 0;

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Lifecycle

	void start()
	{
		//The reader drains a whole step budget's worth of blocks in one burst - measured at
		//around a hundred at a 64k block - and then does nothing until the next frame. The ring
		//has to hold enough to keep that burst fed, or the drain runs dry halfway through and
		//the display is limited by the ring rather than by the GPU.
		//
		//Deeper when backpressuring, because there the ring is the only elastic in the system:
		//the source is paced by whatever the reader takes, so a shallow ring paces it slowly.
		//Without backpressure a shallow ring just means fresher data, which is what a live
		//display wants anyway.
		const std::size_t blocks = backpressure ? 128UZ : 4UZ;
		const std::size_t depth = (buffer_depth > 0U)
			? static_cast<std::size_t>(buffer_depth)
			: blocks * static_cast<std::size_t>(block_size);

		_ring.Resize(depth);
		_reconfigure.store(true, std::memory_order_release);

		//Deliberately no in.max_samples / in.min_samples hint.
		//
		//The obvious move is to ask the port for block_size samples at a time so that a whole
		//batch of transforms arrives at once. It buys nothing here: the ring below is exactly
		//what decouples the size the port delivers from the size the GPU wants, since
		//processBulk() accumulates whatever it is given and draw() drains block_size at a time.
		//Setting it was measured to make no difference to what gets through, and block_size
		//defaults to a megasample, far more than the port's buffer holds, so constraining the
		//scheduler by it could only ever cost. Let the scheduler pick its own chunking.

		//A headless run is legitimate, but it is almost never what someone who built a display
		//into their flowgraph meant. Say so once, at the only point where it is knowable.
		//
		//A log line and not emitErrorMessage(): that puts the block into the ERROR lifecycle
		//state, which aborts the whole flowgraph before a single sample moves. Running without
		//a window is a supported mode, not a failure.
		if(!::imcufosphor::globalRenderHost().Available())
		{
			LogNotice("AnalyzerSink '%s': no RenderHost published, running headless "
				"(samples will be consumed and dropped)\n", this->name.value.c_str());
		}
	}

	/**
		@brief Deliberately does nothing

		GPU resources belong to the render thread and stop() runs on a scheduler thread.
		Destroying a Vulkan object from the wrong thread, or after the window it was created
		against is gone, is the teardown fault DESIGN.md section 17 records. The host calls
		ReleaseGpuResources() from its render loop instead.
	 */
	void stop() {}

	/**
		@brief Drops the GPU resources

		Render thread only, and only after the scheduler has been joined. Called by the host
		before it destroys the window.
	 */
	void ReleaseGpuResources()
	{
		_pane.reset();
		_engine.reset();
		_initialised = false;

		//The pane held a pointer to _annotationSet. It is gone, so the next one built has to be
		//given the set again rather than inheriting a flag saying it already has it.
		_annotationsAttached = false;
	}

	//Deliberately no destructor. gr::Block runs a sequenced constructor that wires up
	//reflection, the settings pipeline and the port buffers, and declaring any of the special
	//members here suppresses the implicit move the graph machinery needs. The unique_ptrs
	//clean themselves up; a host that wants the teardown to happen on the render thread calls
	//ReleaseGpuResources() there first, which is the documented contract above.

	void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings)
	{
		//Scheduler thread. No Vulkan, no allocation of anything the GPU touches, no reading
		//_engine. One thing happens: draw() is told to reconsider everything next time it runs.
		std::ignore = newSettings;
		_reconfigure.store(true, std::memory_order_release);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Data

	[[nodiscard]] work::Status processBulk(InputSpanLike auto& dataIn) noexcept
	{
		const std::size_t n = dataIn.size();

		if(const auto meta = detail::metaFromTags(dataIn.tags(), ignore_tag_sample_rate); meta.Any())
		{
			if(meta.sampleRate.has_value())
				_metaRate.store(*meta.sampleRate, std::memory_order_relaxed);
			if(meta.centerHz.has_value())
				_metaCenter.store(*meta.centerHz, std::memory_order_relaxed);
			_status.Absorb(meta);

			_metaDirty.store(true, std::memory_order_release);
		}

		const std::span<const T> samples(dataIn.data(), n);

		//Stream index this block's first sample will land on, read before the push advances the
		//counter. Annotation tags are placed against it, so sampling it afterwards would put
		//every annotation a whole block late.
		const std::uint64_t streamBase = _ring.Pushed();

		if(backpressure)
		{
			//Consume only what was accepted. What is left stays in the port, so the upstream
			//block is asked for it again rather than having it thrown away - which for a file
			//means the whole recording eventually gets analysed instead of a sample of it.
			const std::size_t taken = _ring.PushUpTo(samples);
			CollectAnnotations(dataIn.tags(), streamBase, taken);
			std::ignore = dataIn.consume(taken);

			//Back off briefly when the ring is full rather than returning straight away having
			//consumed nothing, because the scheduler re-invokes a block that returns OK
			//immediately and the resulting spin hammers the ring's writer sequence.
			//
			//Kept short deliberately. The reader drains in bursts - a step budget's worth every
			//frame - and the writer has to refill between them, so a long sleep here throttles
			//the whole display rather than just this thread: at 250 us it held the pipeline to
			//26 MS/s against a measured ceiling of ~490.
			if((taken == 0) && (n > 0))
				std::this_thread::sleep_for(std::chrono::microseconds(20));

			return work::Status::OK;
		}

		//Copies what fits and counts the rest as dropped. The ring is only drained a whole
		//block at a time, so partial writes are fine: the reader waits until one has
		//accumulated.
		_ring.Push(samples);
		CollectAnnotations(dataIn.tags(), streamBase, _ring.Pushed() - streamBase);

		//The whole span regardless. See the class doc: without backpressure the display can
		//never stall the flowgraph.
		std::ignore = dataIn.consume(n);
		return work::Status::OK;
	}

	/**
		@brief Converts this block's annotation tags and stages them for the render thread

		Scheduler thread. @p accepted is how many of this block's samples actually reached the
		ring, and a tag past that is skipped rather than clamped, because the samples it
		describes are not going to be displayed:

		- Backpressuring, the remainder stays in the port and the same tag arrives again next
		  call with an index relative to the new span, so skipping loses nothing.
		- Not backpressuring, those samples were dropped outright, and an annotation over
		  samples nobody will see is worse than no annotation at all.
	 */
	template<typename TagRange>
	void CollectAnnotations(TagRange&& tags, std::uint64_t streamBase, std::size_t accepted) noexcept
	{
		if(!annotations || (accepted == 0))
			return;

		for(const auto& entry : tags)
		{
			//Signed, and genuinely can be negative: the port reports a tag's index relative to
			//the current stream position, and one published before it reads below zero.
			const std::ptrdiff_t index = std::get<0>(entry);
			if((index < 0) || (static_cast<std::size_t>(index) >= accepted))
				continue;

			const auto& map = std::get<1>(entry).get();
			if(!detail::isAnnotationTag(map))
				continue;

			auto ann = detail::annotationFromTag(
				map, static_cast<std::int64_t>(streamBase + static_cast<std::uint64_t>(index)));
			if(!ann.has_value())
				continue;

			const std::lock_guard<std::mutex> lock(_annotationMutex);

			//Bounded because the writer is faster than the reader by construction: a headless
			//run never drains this at all, and a recording looping for an hour would otherwise
			//stage every replay of every annotation forever.
			if(_annotationStaging.size() >= g_maxStagedAnnotations)
			{
				_annotationsDropped.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			_annotationStaging.push_back(std::move(*ann));
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Display

	[[nodiscard]] work::Status draw(const property_map& /*config*/ = {}) noexcept
	{
		const auto status = [this] {
			return lifecycle::isShuttingDown(this->state()) ? work::Status::DONE : work::Status::OK;
		};

		auto& host = ::imcufosphor::globalRenderHost();
		if(!host.Available())
			return status();	//headless: allocate nothing, draw nothing

		if(!_initialised)
		{
			//First frame on the render thread, which is the earliest point at which the Vulkan
			//context is known to exist and to be ours to use
			_engine = std::make_unique<SpectrumEngine>(EnginePart::All, host.RenderQueue());
			_pane = std::make_unique<AnalyzerPane>(_engine.get(), host.Textures(), color_map);
			_initialised = true;
			_reconfigure.store(true, std::memory_order_release);
		}

		const bool metaChanged = _metaDirty.exchange(false, std::memory_order_acq_rel);
		if(_reconfigure.exchange(false, std::memory_order_acq_rel) || metaChanged)
			ApplyConfiguration();

		_pane->SetSpectrumFraction(spectrum_fraction);

		//Before Render(), because the overlay draws from the set inside it. Cheap when nothing
		//arrived, which is every frame on a stream that has no annotations.
		DrainAnnotations();

		//timeout_ms rate-limits the GPU work, and nothing else.
		//
		//It must not skip the ImGui calls below. ImGui is immediate mode: a window that is not
		//submitted on a frame is not drawn on that frame, so returning early here blanks the
		//whole pane until the next draw that gets through - which at a 33 ms limit against a
		//16.7 ms vsync is every other frame, i.e. a 30 Hz flicker. Skipping the compute is
		//free by comparison: the texture still holds what the last tone map wrote, so the
		//display simply shows the previous frame's image again.
		bool advance = true;
		if(timeout_ms > 0ULL)
		{
			const auto now = std::chrono::steady_clock::now();
			if(now < _lastDraw + std::chrono::milliseconds(timeout_ms))
				advance = false;
			else
				_lastDraw = now;
		}

		if(advance)
		{
			//Frozen after end_of_stream: keep drawing the last state, stop advancing it.
			//Draining the ring here would scroll the tail of the recording off the top of the
			//waterfall.
			const double tDrain0 = GetTime();
			_lastDrainSteps = _status.Frozen() ? 0 : StepFromRing();
			const double tDrain1 = GetTime();
			_lastDrainMs = (tDrain1 - tDrain0) * 1000.0;

			//Tone map, then draw. Recorded into the host's scratch buffer and submitted right
			//here rather than batched to the end of the frame: the ImGui call below samples
			//the texture this writes, and the window's render pass is already being recorded,
			//so anything deferred would land after the pass that reads it.
			//
			//No cross-submit synchronisation is needed against StepFromRing() above because
			//SpectrumEngine::Step() ends in SubmitAndBlock. If Step() is ever made
			//non-blocking, this becomes a real barrier problem rather than an ordering
			//convention.
			if(auto* cmdBuf = host.FrameComputeCommandBuffer())
			{
				const double tTone0 = GetTime();
				cmdBuf->begin({});
				_pane->ToneMap(*cmdBuf);
				cmdBuf->end();
				host.RenderQueue()->SubmitAndBlock(*cmdBuf);
				_lastToneMapMs = (GetTime() - tTone0) * 1000.0;
			}
		}

		ImGui::Begin(this->name.value.c_str());
		RenderControls();
		const auto avail = ImGui::GetContentRegionAvail();
		if((avail.x > 0) && (avail.y > 0))
			_pane->Render(avail);
		ImGui::End();

		return status();
	}

protected:
	/**
		@brief Moves staged annotations into the set the overlay draws from

		Render thread. The set is attached to the pane on the first arrival rather than up
		front: a graph with no annotation tags then leaves the pane exactly as it was, and
		AnalyzerPane already treats a null set as "nothing to draw" rather than as an error.
	 */
	void DrainAnnotations()
	{
		//A per-frame snapshot, like SetSpectrumFraction above. AnnotationOverlay is constructed
		//disabled - sigmf-spectrum turns it on from a checkbox in MainWindow - so a sink that
		//never touched this would collect annotations correctly and draw none of them. Driving
		//it from the setting every frame is also what makes the runtime checkbox work without a
		//separate apply path; the collected set is kept either way, so switching it back on does
		//not mean replaying the recording.
		_pane->GetOverlay().SetEnabled(annotations);

		if(!annotations)
			return;

		//The lock covers a vector swap and nothing else. Converting, sorting and pruning all
		//happen outside it, so the scheduler thread is never blocked behind this frame's work.
		_annotationScratch.clear();
		{
			const std::lock_guard<std::mutex> lock(_annotationMutex);
			_annotationStaging.swap(_annotationScratch);
		}

		if(!_annotationScratch.empty())
		{
			for(const auto& a : _annotationScratch)
				_annotationSet.Add(a);

			//Add() invalidates the running-maximum prefix the overlap query binary searches, so
			//this has to run before anything reads the set - i.e. before Render() below.
			_annotationSet.Finalize();

			PruneAnnotations();
		}

		if(!_annotationsAttached && !_annotationSet.empty())
		{
			//No RecordingClock. The second half of SetAnnotationSource is the wall clock a
			//recording pins its samples to, and a stream arriving on a port has no equivalent -
			//inventing one from the host's clock would date the data to when it was displayed.
			//WaterfallArea null checks it and omits the timestamp from its hover readout.
			_pane->SetAnnotationSource(&_annotationSet, nullptr);
			_annotationsAttached = true;
		}
	}

	/**
		@brief Drops annotations that have scrolled past the oldest row the waterfall still holds

		A repeating source republishes its entire annotation schedule on every wrap, so without
		this the set grows for as long as the graph runs. dect6 carries 2000 annotations over ten
		seconds, which is an ordinary recording rather than a pathological one; an hour of
		looping it would otherwise be 720k annotations, every one of them queried per frame.

		Cutting at the oldest row loses nothing that could have been drawn: the waterfall cannot
		show a row it has already scrolled away, and the spectrum only ever draws the current
		block.
	 */
	void PruneAnnotations()
	{
		if(_annotationSet.size() <= g_maxLiveAnnotations)
			return;

		auto& rows = _engine->GetRowHistory();
		const std::size_t count = rows.GetCount();
		if(count == 0)
			return;

		RowMark oldest;
		if(!rows.GetRow(count - 1, oldest))
			return;

		_annotationScratch.clear();
		for(std::size_t i=0; i<_annotationSet.size(); i++)
		{
			const auto& a = _annotationSet[i];

			//EffectiveEnd rather than sampleEnd, so a zero length annotation is tested the same
			//way the overlap query tests it and the two cannot disagree about what is visible.
			if(AnnotationSet::EffectiveEnd(a) >= oldest.streamStart)
				_annotationScratch.push_back(a);
		}

		//Everything is still on screen. That is a display holding more annotations than the cap
		//expected, not a leak, and clearing the set would erase what is being looked at.
		if(_annotationScratch.size() == _annotationSet.size())
			return;

		_annotationSet.Clear();
		for(const auto& a : _annotationScratch)
			_annotationSet.Add(a);
		_annotationSet.Finalize();
		_annotationScratch.clear();
	}

	/**
		@brief The status line and the settings panel

		Every control here writes back through settings().setStaged() rather than assigning the
		Annotated member directly. That matters: setStaged is the one path that also notifies
		settingsChanged(), keeps the value visible to the message bus and to anything else
		inspecting the block, and applies it at a point the scheduler considers safe. Writing
		the member would change the display and leave the block's declared settings lying.
	 */
	void RenderControls()
	{
		char detail[128] = "starting";
		if(_engine)
		{
			int len = snprintf(detail, sizeof(detail), "%" PRId64 " rows | %" PRId64 " spectra",
				_engine->GetRowsPlayed(), _engine->GetStats().spectra);

			//Only once there are any. A count of zero on every graph that has no annotations
			//would be noise in the one line the user actually reads.
			if((len > 0) && (len < static_cast<int>(sizeof(detail))) && !_annotationSet.empty())
			{
				const std::uint64_t lost = _annotationsDropped.load(std::memory_order_relaxed);
				if(lost > 0)
				{
					snprintf(detail + len, sizeof(detail) - static_cast<size_t>(len),
						" | %zu annotations (%" PRIu64 " dropped)", _annotationSet.size(), lost);
				}
				else
				{
					snprintf(detail + len, sizeof(detail) - static_cast<size_t>(len),
						" | %zu annotations", _annotationSet.size());
				}
			}
		}

		detail::RenderStreamStatus(_status, _ring.Pushed(), _ring.Dropped(), detail);

		if(!ImGui::CollapsingHeader("Settings"))
			return;

		property_map staged;

		int fft = static_cast<int>(fft_size);
		ImGui::SetNextItemWidth(140 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::InputInt("FFT size", &fft, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && (fft > 0))
			staged["fft_size"] = gr::Size_t(fft);

		int group = static_cast<int>(group_size);
		ImGui::SetNextItemWidth(140 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::SliderInt("Spectra/row", &group, 1, 4096))
			staged["group_size"] = gr::Size_t(group);

		float lo = db_min;
		float hi = db_max;
		ImGui::SetNextItemWidth(220 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::DragFloatRange2("dBm range", &lo, &hi, 1.0f, -200.0f, 30.0f))
		{
			staged["db_min"] = lo;
			staged["db_max"] = hi;
		}

		float frac = spectrum_fraction;
		ImGui::SetNextItemWidth(140 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::SliderFloat("Split", &frac, 0.05f, 0.95f))
			staged["spectrum_fraction"] = frac;

		double centre = center_frequency;
		ImGui::SetNextItemWidth(220 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::InputDouble("Centre Hz", &centre, 0, 0, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue))
			staged["center_frequency"] = centre;

		//Exposed mainly because it is the knob that decides how much of the stream gets shown,
		//and because raising it is the quickest way to confirm the pane redraws every frame
		//regardless: at 200 ms the picture should update slowly and stay put, not blink.
		int refresh = static_cast<int>(timeout_ms);
		ImGui::SetNextItemWidth(140 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::SliderInt("Refresh ms", &refresh, 0, 250) && (refresh >= 0))
			staged["timeout_ms"] = static_cast<std::uint64_t>(refresh);

		float budget = step_budget_ms;
		ImGui::SetNextItemWidth(140 * ::imcufosphor::globalRenderHost().DpiScale());
		if(ImGui::SliderFloat("Step budget ms", &budget, 1.0f, 100.0f, "%.0f"))
			staged["step_budget_ms"] = budget;

		bool bp = backpressure;
		if(ImGui::Checkbox("Backpressure (files only)", &bp))
			staged["backpressure"] = bp;

		bool ann = annotations;
		if(ImGui::Checkbox("Annotations", &ann))
			staged["annotations"] = ann;

		if(!staged.empty())
			std::ignore = this->settings().setStaged(staged);
	}

	///@brief Pushes the current settings into the engine. Render thread only.
	void ApplyConfiguration()
	{
		EngineConfig cfg = _engine->GetConfig();

		cfg.fftLength = static_cast<int64_t>(fft_size);
		cfg.blockSize = static_cast<int64_t>(block_size);
		cfg.groupSize = static_cast<int64_t>(group_size);
		cfg.window = WindowFromName(window);
		cfg.rangeMin = db_min;
		cfg.rangeMax = db_max;
		cfg.sampleScale = sample_scale;

		//A tagged rate beats the setting, which is only a fallback for a stream that never
		//says. Same for the centre frequency.
		const double taggedRate = _metaRate.load(std::memory_order_relaxed);
		cfg.sampleRate = (taggedRate > 0) ? taggedRate : static_cast<double>(sample_rate);

		const double taggedCenter = _metaCenter.load(std::memory_order_relaxed);
		cfg.centerFrequency = (taggedCenter != 0.0) ? taggedCenter : center_frequency.value;

		_engine->Configure(cfg);
	}

	/**
		@brief Runs one block through the engine if a whole one is available

		At most one block per frame. An unbounded drain here would let a fast producer keep the
		render thread inside this function indefinitely, turning a display into a stall; the
		bounded version degrades to dropping in processBulk() instead, which is where the drop
		is counted and visible.
	 */
	/**
		@brief Pushes as many whole blocks through the GPU as the frame's budget allows

		@return Blocks stepped

		One block per frame was the original mistake: it caps the display at block_size times
		the frame rate regardless of how fast the pipeline is. Draining to a wall-clock budget
		instead is what MainWindow has always done, and it is the difference between 2 MS/s and
		something near the pipeline's own ceiling.

		Always steps at least once when a block is available, so that a budget smaller than one
		block's cost still makes progress rather than deadlocking the display at zero.
	 */
	std::size_t StepFromRing()
	{
		const std::size_t want = static_cast<std::size_t>(block_size);
		const double deadline = GetTime() + std::max(0.0f, step_budget_ms.value) / 1000.0;

		std::size_t stepped = 0;
		while(true)
		{
			//The callback form keeps the reader span alive across the engine step: it points
			//into the ring, and consuming before the GPU upload has finished would let the
			//writer overwrite data in flight.
			const bool got = _ring.WithBlock(want,
				[this](std::span<const T> samples)
				{
					_engine->Step(samples);
				});

			if(!got)
				break;

			stepped++;
			if(GetTime() >= deadline)
				break;
		}

		return stepped;
	}

	///@brief Maps a window name onto the filter's enumeration, defaulting to Blackman-Harris
	static WindowFunction WindowFromName(const std::string& name)
	{
		if(name == "rectangular")
			return WINDOW_RECTANGULAR;
		if(name == "hamming")
			return WINDOW_HAMMING;
		if(name == "hann")
			return WINDOW_HANN;
		if(name == "blackman-harris")
			return WINDOW_BLACKMAN_HARRIS;

		//An unrecognised name is a typo in a flowgraph, not a reason to refuse to display
		//anything. Blackman-Harris is the default the application uses.
		return WINDOW_BLACKMAN_HARRIS;
	}
};

} // namespace gr::imcufosphor

#endif
