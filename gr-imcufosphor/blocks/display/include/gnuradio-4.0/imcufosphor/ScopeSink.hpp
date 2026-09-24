/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of ScopeSink
 */
#ifndef GR_IMCUFOSPHOR_SCOPE_SINK_HPP
#define GR_IMCUFOSPHOR_SCOPE_SINK_HPP

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <gnuradio-4.0/imcufosphor/detail/IqRing.hpp>
#include <gnuradio-4.0/imcufosphor/detail/MetaFromTags.hpp>
#include <gnuradio-4.0/imcufosphor/detail/StreamStatus.hpp>

#include "RenderHost.h"
#include "ScopeArea.h"
#include "ScopeCapture.h"

namespace gr::imcufosphor {

using namespace gr;

GR_REGISTER_BLOCK("gr::imcufosphor::ScopeSink", gr::imcufosphor::ScopeSink, ([T]),
	[ float, std::complex<float> ])

/**
	@brief Triggered time domain display with a configurable number of input ports

	One overlaid plot, every port on it. A complex port draws two traces, I and Q; a real port
	draws one. The port count is fixed at construction through the @c n_inputs setting.

	@par Connecting it

	gnuradio4's compile-time connect is a hard error on a vector of ports
	(Graph.hpp:594-596: "Vector size is not known at compile-time"). Use the runtime form:

	@code
	auto& sink = fg.emplaceBlock<ScopeSink<std::complex<float>>>({{"n_inputs", gr::Size_t{2}}});
	std::ignore = fg.connect(src0, "out", sink, "in#0");
	std::ignore = fg.connect(src1, "out", sink, "in#1");
	@endcode

	Every declared port must be connected. A synchronous port that is not reports zero samples
	available, which makes the minimum across the block's ports zero, which means the block never
	runs at all - a silent hang rather than an error, so start() checks and says so.

	@par Where the work happens

	The trigger search and the record assembly are in ScopeCapture and ScopeTriggerEngine, which
	is also where the tests are. This block is the handoff: rings on the scheduler side, drain and
	draw on the render side, and the settings that connect the two.

	@par Threading

	Identical to the other sinks. processBulk() runs on a scheduler thread and may only push into
	the lock-free rings; everything touching Vulkan or the capture happens in draw(), on the
	render thread. See DEVELOPERS.md section 4.

	@par Backpressure

	None, deliberately, and unlike AnalyzerSink there is not even an option. A display must never
	stall a flowgraph, and a scope is a sampling instrument by nature: it shows one record per
	sweep and discards the rest whatever the buffering. Making the source wait for a display that
	is going to throw the samples away regardless would slow the graph to no purpose.
 */
template<typename T>
requires std::same_as<T, float> || std::same_as<T, std::complex<float>>
struct ScopeSink : Block<ScopeSink<T>, Drawable<UICategory::Content, "ImGui">>
{
	using Description = Doc<R""(@brief Vulkan triggered time-domain display.

Draws a configurable number of input ports as an overlaid oscilloscope plot: two traces (I and
Q) per complex port, one per real port. Supports stop/auto/normal/single run control, an edge
trigger with selectable source, slope, level, hysteresis and holdoff, and a magnitude operator
for triggering on an IQ burst envelope.

Port count is fixed by n_inputs at construction and cannot change once anything is connected.
Connect with the runtime form, graph.connect(src, "out", sink, "in#0").

Requires a host application to have published an imcufosphor RenderHost; without one it runs
headless, consuming and dropping samples.)"">;

	template<typename U, gr::meta::fixed_string TDescription = "", typename... Arguments>
	using A = Annotated<U, TDescription, Arguments...>;

	///@brief Floats per sample: two for an interleaved complex port, one for a real one
	static constexpr std::size_t kComponents = std::same_as<T, std::complex<float>> ? 2UZ : 1UZ;

	//The reinterpret_cast in Drain() rests on this. std::complex<float> is specified to be two
	//adjacent floats; qa_IqLayout pins it for this project's standard library.
	static_assert(sizeof(T) == kComponents * sizeof(float), "sample type is not packed floats");

	std::vector<PortIn<T>> in{1UZ};

	A<gr::Size_t, "n_inputs", Visible, Doc<"Input ports. Fixed at construction.">, Limits<1U, 16U>>
		n_inputs = 1U;

	A<gr::Size_t, "record_length", Visible, Doc<"Samples per acquisition">>
		record_length = 16384U;

	A<float, "pretrigger", Visible, Doc<"Fraction of the record before the trigger, 0 to 1">>
		pretrigger = 0.5f;

	A<gr::Size_t, "history_depth", Doc<"Samples of pre-trigger history per port. 0 means four records.">>
		history_depth = 0U;

	A<float, "sample_rate", Visible, Unit<"Hz">, Doc<"Fallback sample rate, used until a tag supplies one">>
		sample_rate = 1.0f;

	A<bool, "ignore_tag_sample_rate", Doc<"Use the sample_rate setting even when the stream tags one">>
		ignore_tag_sample_rate = false;

	A<std::string, "trigger_mode", Visible, Doc<"stop | auto | normal | single">>
		trigger_mode = "auto";

	A<std::string, "trigger_kind", Visible, Doc<"edge | free">>
		trigger_kind = "edge";

	A<std::string, "trigger_slope", Visible, Doc<"rising | falling | any">>
		trigger_slope = "rising";

	A<std::string, "trigger_operator", Visible, Doc<"raw | magnitude (|I+jQ| of the source's port)">>
		trigger_operator = "raw";

	A<gr::Size_t, "trigger_source", Visible, Doc<"Trace index the level is tested against">>
		trigger_source = 0U;

	A<float, "trigger_level", Visible, Doc<"Trigger threshold, in input units">>
		trigger_level = 0.0f;

	A<float, "trigger_hysteresis", Visible, Doc<"Dead band half-width around the level">>
		trigger_hysteresis = 0.0f;

	A<std::uint64_t, "holdoff_fs", Unit<"fs">, Doc<"Minimum time between accepted triggers">>
		holdoff_fs = 0ULL;

	A<std::uint64_t, "auto_timeout_ms", Unit<"ms">, Doc<"How long Auto waits before forcing a sweep">>
		auto_timeout_ms = 100ULL;

	A<std::uint32_t, "trace_mask", Visible, Doc<"Bitmask of traces to draw">>
		trace_mask = 0xffffffffU;

	A<float, "volts_per_div", Visible, Doc<"Full scale is eight divisions of pane height">>
		volts_per_div = 0.25f;

	A<float, "persistence", Visible, Doc<"Display persistence, 0 to 1">>
		persistence = 0.0f;

	A<std::uint64_t, "timeout_ms", Unit<"ms">, Doc<"Minimum interval between draws. 0 draws every frame">>
		timeout_ms = 33ULL;

	A<gr::Size_t, "buffer_depth", Doc<"Handoff ring capacity in samples. 0 means four records.">>
		buffer_depth = 0U;

	GR_MAKE_REFLECTABLE(ScopeSink, in, n_inputs, record_length, pretrigger, history_depth,
		sample_rate, ignore_tag_sample_rate, trigger_mode, trigger_kind, trigger_slope,
		trigger_operator, trigger_source, trigger_level, trigger_hysteresis, holdoff_fs,
		auto_timeout_ms, trace_mask, volts_per_div, persistence, timeout_ms, buffer_depth);

	////////////////////////////////////////////////////////////////////////////////////////////////
	// State

	/**
		@brief Everything owned per input port

		Held by pointer and built once in start(). Both members are non-movable - IqRing holds
		reader and writer handles into its own buffer, and an atomic is neither copyable nor
		movable - so a vector of them could be sized but never resized, which is a distinction
		nobody wants to have to remember.
	 */
	struct PortState
	{
		detail::IqRing<T> ring;

		///@brief Sample rate this port's tags reported, or 0
		std::atomic<double> rate{0.0};
	};

	std::vector<std::unique_ptr<PortState>> _ports;

	/**
		@brief Stream index at which samples stop being spliceable to the ones before

		Monotonic, written by the scheduler thread when a ring refuses part of a block and read
		once per frame by the render thread. Counted in accepted samples, which is the same
		coordinate ScopeHistory::End() counts in, because everything accepted is eventually
		appended.
	 */
	std::atomic<std::uint64_t> _discontinuity{0};

	///@brief Samples accepted into every ring so far. Scheduler thread.
	std::atomic<std::uint64_t> _accepted{0};

	/**
		@brief Samples consumed but not displayed, because a ring had no room

		Counted here rather than inside the rings. IqRing::Push() drops internally and keeps its
		own tally, but joint admission cannot use it: Push discards where the caller cannot see
		how much, which is exactly what would let the ports drift apart. PushUpTo() reports
		instead, and the block owns the count - so IqRing::Dropped() stays zero for the whole
		life of this block, and this is the number that means anything.
	 */
	std::atomic<std::uint64_t> _dropped{0};

	std::atomic<bool> _reconfigure{true};
	std::atomic<bool> _metaDirty{false};

	///@brief True once the initial settings have been applied. See settingsChanged().
	bool _portsFixed = false;

	///@brief Aggregated across ports: whether anything went wrong, not which port it went wrong on
	detail::StreamStatus _status;

	//Render thread only from here down
	std::unique_ptr<ScopeCapture> _capture;
	std::unique_ptr<ScopeArea> _area;
	bool _initialised = false;
	std::chrono::steady_clock::time_point _lastDraw{};

	///@brief True when the ports disagree about the sample rate, for the status line
	bool _rateConflict = false;

	///@brief Sweep count and when it last moved, so a stalled trigger can say so
	///@brief Last volts_per_div pushed to the area, so a re-apply does not stamp on the UI
	float _lastAppliedVoltsPerDiv = -1.0f;
	std::size_t _lastAppliedTraceCount = 0;

	std::uint64_t _lastSeenSweeps = 0;
	std::chrono::steady_clock::time_point _lastSweepChange{};
	bool _haveSweepClock = false;

	////////////////////////////////////////////////////////////////////////////////////////////////
	// Lifecycle

	void start()
	{
		const std::size_t nports = std::max<std::size_t>(1, static_cast<std::size_t>(n_inputs));

		const std::size_t depth = (buffer_depth > 0U)
			? static_cast<std::size_t>(buffer_depth)
			: 4UZ * static_cast<std::size_t>(record_length);

		_ports.clear();
		for(std::size_t i = 0; i < nports; i++)
		{
			auto p = std::make_unique<PortState>();
			p->ring.Resize(depth);
			_ports.push_back(std::move(p));
		}

		//A synchronous port that is never connected reports zero available, the minimum across the
		//block's ports is then zero, and the block simply never runs. That presents as a hang with
		//no output anywhere, so say it plainly instead.
		for(std::size_t i = 0; i < in.size(); i++)
		{
			if(!in[i].isConnected())
			{
				LogError("ScopeSink '%s': input port %zu is not connected. Every declared port must "
					"be, or the block will never run.\n", this->name.value.c_str(), i);
			}
		}

		_reconfigure.store(true, std::memory_order_release);

		if(!::imcufosphor::globalRenderHost().Available())
		{
			LogNotice("ScopeSink '%s': no RenderHost published, running headless "
				"(samples will be consumed and dropped)\n", this->name.value.c_str());
		}
	}

	///@brief Nothing: GPU teardown belongs to the render thread. See AnalyzerSink::stop().
	void stop() {}

	///@brief Drops the GPU resources. Render thread only, after the scheduler has been joined.
	void ReleaseGpuResources()
	{
		_area.reset();
		_capture.reset();
		_initialised = false;
	}

	void settingsChanged(const property_map& /*oldSettings*/, const property_map& newSettings)
	{
		//The type-erased port table takes non-owning references into this vector the first time
		//anything asks for it (BlockModel.hpp:673-700), which for a sink is when the scheduler
		//resolves its edges. Resizing after that dangles every one of them.
		//
		//The obvious guard - refuse once a port reports isConnected() - does not work here.
		//Port::isConnected() for an input is "the buffer has a writer", which this project's
		//gnuradio4 only makes true when the scheduler starts running: measured false after
		//connect(), and still false after exchange(), by which point the references already
		//exist. A guard that opens after the window it protects is worse than none, because it
		//reads like protection.
		//
		//So n_inputs is construction-time only, latched on the first settings application - which
		//is the one init() makes from the emplaceBlock property map. That is what the setting's
		//documentation promises, it needs no knowledge of when the graph materialises ports, and
		//it cannot be too late.
		if(newSettings.contains("n_inputs"))
		{
			const std::size_t want = std::max<std::size_t>(1, static_cast<std::size_t>(n_inputs));

			if(_portsFixed)
			{
				if(want != in.size())
				{
					throw gr::exception("ScopeSink: n_inputs is fixed at construction. Pass it in "
						"the emplaceBlock property map rather than setting it afterwards.");
				}
			}
			else if(want != in.size())
				in.resize(want);
		}
		_portsFixed = true;

		//Scheduler thread: raise a flag, touch nothing the GPU owns
		_reconfigure.store(true, std::memory_order_release);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// Ingest

	template<gr::InputSpanLike TInSpan>
	[[nodiscard]] work::Status processBulk(std::span<TInSpan>& ins) noexcept
	{
		if(ins.empty() || _ports.empty())
			return work::Status::INSUFFICIENT_INPUT_ITEMS;

		//gnuradio4's own vector-port sinks take the minimum rather than trusting the ports to be
		//in step (studio/StudioSeriesSink.hpp:677). One min, and a whole failure class is gone.
		std::size_t n = std::numeric_limits<std::size_t>::max();
		for(const auto& s : ins)
			n = std::min(n, s.size());

		if(n == 0)
			return work::Status::INSUFFICIENT_INPUT_ITEMS;

		const std::size_t nports = std::min(ins.size(), _ports.size());

		//One admission decision for every port. Each ring accepts exactly the same count, so every
		//ring holds exactly the same stream indices at all times and alignment is not something
		//that has to be maintained - it is structurally impossible to lose.
		std::size_t room = n;
		for(std::size_t p = 0; p < nports; p++)
			room = std::min(room, _ports[p]->ring.Room());

		if(room > 0)
		{
			//PushUpTo rather than Push: Push drops inside the ring, where the caller cannot see
			//how much, which is exactly what would break the joint decision.
			for(std::size_t p = 0; p < nports; p++)
			{
				const auto taken = _ports[p]->ring.PushUpTo(
					std::span<const T>(ins[p].data(), room));
				room = std::min(room, taken);
			}
		}

		const auto before = _accepted.fetch_add(room, std::memory_order_relaxed);

		if(room < n)
		{
			_dropped.fetch_add(n - room, std::memory_order_relaxed);

			//Samples were lost here, so nothing after this point is adjacent in time to anything
			//before it. Published monotonically; the render thread refuses any record spanning it.
			auto cur = _discontinuity.load(std::memory_order_relaxed);
			const auto mark = before + room;
			while((cur < mark) &&
				!_discontinuity.compare_exchange_weak(cur, mark, std::memory_order_relaxed))
			{
			}
		}

		//The whole span regardless of what was accepted. A display never stalls a flowgraph.
		for(auto& s : ins)
			std::ignore = s.consume(n);

		return work::Status::OK;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// Display

	[[nodiscard]] work::Status draw(const property_map& /*config*/ = {}) noexcept
	{
		const auto status = [this] {
			return lifecycle::isShuttingDown(this->state()) ? work::Status::DONE : work::Status::OK;
		};

		auto& host = ::imcufosphor::globalRenderHost();
		if(!host.Available())
			return status();

		if(!_initialised)
		{
			_capture = std::make_unique<ScopeCapture>();
			_area = std::make_unique<ScopeArea>(_capture.get(), host.Textures());
			_initialised = true;
			_reconfigure.store(true, std::memory_order_release);
		}

		const bool metaChanged = _metaDirty.exchange(false, std::memory_order_acq_rel);
		if(_reconfigure.exchange(false, std::memory_order_acq_rel) || metaChanged)
			ApplyConfiguration();

		//timeout_ms rate-limits the GPU work and nothing else. It must not skip the ImGui calls:
		//ImGui is immediate mode, so a window not submitted on a frame is not drawn on it, and
		//returning early gives a 30 Hz flicker against a 16.7 ms vsync. See SpectrumSink::draw().
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
			//Frozen after end_of_stream: keep drawing the last record, stop advancing
			if(!_status.Frozen())
			{
				Drain();
				_capture->NoteDiscontinuity(_discontinuity.load(std::memory_order_relaxed));
				_capture->Update(std::chrono::steady_clock::now());
			}

			//Recorded and submitted here rather than deferred: the ImGui call below samples the
			//texture this writes. See RenderHost::FrameComputeCommandBuffer().
			if(auto* cmdBuf = host.FrameComputeCommandBuffer())
			{
				cmdBuf->begin({});
				_area->ToneMap(*cmdBuf);
				cmdBuf->end();
				host.RenderQueue()->SubmitAndBlock(*cmdBuf);
			}
		}

		ImGui::Begin(this->name.value.c_str());
		RenderControls();
		const auto avail = ImGui::GetContentRegionAvail();
		if((avail.x > 0) && (avail.y > 0))
			_area->Render(avail);
		ImGui::End();

		return status();
	}

protected:
	////////////////////////////////////////////////////////////////////////////////////////////////
	// Drain

	/**
		@brief Moves everything the rings hold into the capture's histories

		Takes the same count from every ring, so the histories stay in step. A ring that accepted
		a sample or two more than its neighbours keeps them at its head for the next frame, which
		is what makes the surplus harmless rather than a creeping time shift.

		There is no wall clock budget here, unlike AnalyzerSink::StepFromRing(). That exists
		because every sample it drains costs a GPU submit and it is free to discard what it cannot
		afford. Here a sample costs one memcpy word and one float compare, and discarding is not
		free at all: an edge trigger has to see every sample pair, or the edge it exists to catch
		simply does not happen. The bound is the history depth instead, which ScopeHistory applies
		itself - draining more than it holds is provably pointless.
	 */
	void Drain()
	{
		if(_ports.empty())
			return;

		std::size_t n = std::numeric_limits<std::size_t>::max();
		for(auto& p : _ports)
			n = std::min(n, p->ring.Available());

		if((n == 0) || (n == std::numeric_limits<std::size_t>::max()))
			return;

		for(std::size_t i = 0; i < _ports.size(); i++)
		{
			//A failed drain would silently stall the display, so it is counted rather than
			//ignored - see _drainFailures.
			if(!_ports[i]->ring.WithBlock(n, [this, i](std::span<const T> samples) {
				//The layout guarantee static_assert above pins: complex<float> is float[2], so an
				//interleaved history needs no deinterleave pass on the way in. The strided read
				//happens on the two paths that are rare, the trigger scan and the record copy.
				_capture->Append(i, std::span<const float>(
					reinterpret_cast<const float*>(samples.data()),
					samples.size() * kComponents));
			}))
			{
				_drainFailures++;
			}
		}
	}

public:
	std::uint64_t _drainFailures = 0;
protected:

	////////////////////////////////////////////////////////////////////////////////////////////////
	// Configuration

	/**
		@brief Reconciles the ports' reported sample rates into the one the plot can have

		A single overlaid plot has exactly one time base; there is no representation in which two
		rates are both correct, and resampling to reconcile them would be a signal processing
		decision a display has no business making. Port zero wins, and the status line says so.
	 */
	double ReconcileRate()
	{
		_rateConflict = false;
		if(_ports.empty())
			return static_cast<double>(sample_rate);

		const double r0 = _ports[0]->rate.load(std::memory_order_relaxed);

		for(std::size_t p = 1; p < _ports.size(); p++)
		{
			const double rp = _ports[p]->rate.load(std::memory_order_relaxed);
			if((rp > 0) && (r0 > 0) && (std::abs(rp - r0) > 1e-9 * r0))
			{
				_rateConflict = true;
				break;
			}
		}

		return (r0 > 0) ? r0 : static_cast<double>(sample_rate);
	}

	void ApplyConfiguration()
	{
		ScopeCaptureConfig cfg;
		cfg.recordLength = static_cast<std::size_t>(record_length);
		cfg.historyDepth = static_cast<std::size_t>(history_depth);
		cfg.sampleRate = ReconcileRate();

		cfg.trigger.mode = ModeFromName(trigger_mode);
		cfg.trigger.kind = KindFromName(trigger_kind);
		cfg.trigger.slope = SlopeFromName(trigger_slope);
		cfg.trigger.op = OperatorFromName(trigger_operator);
		cfg.trigger.source = static_cast<std::size_t>(trigger_source);
		cfg.trigger.level = trigger_level;
		cfg.trigger.hysteresis = trigger_hysteresis;
		cfg.trigger.holdoffFs = static_cast<std::int64_t>(holdoff_fs);
		cfg.trigger.autoTimeoutMs = static_cast<std::int64_t>(auto_timeout_ms);
		cfg.trigger.pretrigger = pretrigger;

		//From n_inputs, not from _ports.size(). The rings are built in start(), which the
		//scheduler calls on its own thread whenever it gets round to it - so the first draw() can
		//easily arrive first, and sizing the capture from an empty _ports gave it ONE port. Every
		//trace past the first port then kept the default vertical scale, because it did not exist
		//at the moment the settings were applied. n_inputs is fixed at construction and is correct
		//from the very first frame.
		const std::size_t nports = std::max<std::size_t>(1, static_cast<std::size_t>(n_inputs));
		_capture->Configure(nports, kComponents, cfg);

		_area->SetTraceMask(trace_mask);
		_area->SetPersistDecay(persistence);

		//The block setting is the starting scale a flowgraph opens at, applied to every trace.
		//Only pushed when it has actually changed: ApplyConfiguration() also runs whenever a tag
		//updates the sample rate, and re-stamping the setting every time would undo a V/div the
		//user had just dialled in on the pane.
		//Re-applied when the trace count changes as well as when the value does. Guarding on the
		//value alone meant a later reconfiguration that grew the trace list left the new traces on
		//the default scale forever.
		const std::size_t ntraces = _area->GetTraceCount();
		if((volts_per_div != _lastAppliedVoltsPerDiv) || (ntraces != _lastAppliedTraceCount))
		{
			_lastAppliedVoltsPerDiv = volts_per_div;
			_lastAppliedTraceCount = ntraces;
			for(std::size_t i = 0; i < ntraces; i++)
				_area->SetVoltsPerDiv(i, volts_per_div);
		}

		_area->RequestFit();
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// UI

	void RenderControls()
	{
		auto& eng = _capture->GetTriggerEngine();

		//The in-pane controls write back through setStaged() rather than assigning the Annotated
		//member. That is the one path that also notifies settingsChanged(), keeps the value
		//visible on the message bus, and applies it where the scheduler considers it safe.
		//Assigning the member would change the display and leave the block's settings lying.
		property_map staged;

		const char* modes[] = {"stop", "auto", "normal", "single"};
		for(const char* m : modes)
		{
			const bool active = (trigger_mode.value == m);
			if(active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if(ImGui::Button(m))
				staged["trigger_mode"] = std::string(m);
			if(active)
				ImGui::PopStyleColor();
			ImGui::SameLine();
		}

		if(ImGui::Button("Force"))
			eng.RequestForce();
		ImGui::SameLine();
		if(ImGui::Button("Arm"))
			eng.Rearm();

		ImGui::SameLine();
		ImGui::TextUnformatted(eng.GetStateText());

		ImGui::SameLine();
		ImGui::Text("| %" PRIu64 " sweeps", static_cast<std::uint64_t>(eng.GetCaptureCount()));

		//An armed trigger that finds nothing is a legitimate state - it is what Normal mode is
		//for - but it is indistinguishable from a hung display unless the pane says which it is.
		//Worth the four lines: the first time this happened in earnest it took a bisected source,
		//a ring test and a duplicate-sample probe to establish that the scope was working
		//correctly and the signal had gone flat.
		{
			const auto sweeps = eng.GetCaptureCount();
			const auto nowUi = std::chrono::steady_clock::now();
			if((sweeps != _lastSeenSweeps) || !_haveSweepClock)
			{
				_lastSeenSweeps = sweeps;
				_lastSweepChange = nowUi;
				_haveSweepClock = true;
			}

			const double stalledSec = std::chrono::duration<double>(nowUi - _lastSweepChange).count();
			if((eng.GetState() == CaptureState::Armed) && (stalledSec > 1.5))
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
					"| no trigger for %.0f s - check the level, the slope and the source",
					stalledSec);
			}
		}
		if(eng.GetMissedCount() > 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "| %" PRIu64 " missed",
				static_cast<std::uint64_t>(eng.GetMissedCount()));
		}

		//Trigger level, and which trace it is tested against
		ImGui::SetNextItemWidth(12 * ImGui::GetFontSize());
		float level = trigger_level;
		if(ImGui::DragFloat("level", &level, 0.01f))
			staged["trigger_level"] = level;

		ImGui::SameLine();
		ImGui::SetNextItemWidth(10 * ImGui::GetFontSize());
		const std::size_t ntraces = _area->GetTraceCount();
		const std::size_t src = std::min<std::size_t>(trigger_source, (ntraces > 0) ? ntraces - 1 : 0);
		if(ImGui::BeginCombo("source", (ntraces > 0) ? _area->GetTraceName(src) : ""))
		{
			for(std::size_t i = 0; i < ntraces; i++)
			{
				if(ImGui::Selectable(_area->GetTraceName(i), i == src))
					staged["trigger_source"] = static_cast<gr::Size_t>(i);
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		ImGui::SetNextItemWidth(8 * ImGui::GetFontSize());
		if(ImGui::BeginCombo("slope", trigger_slope.value.c_str()))
		{
			for(const char* s : {"rising", "falling", "any"})
			{
				if(ImGui::Selectable(s, trigger_slope.value == s))
					staged["trigger_slope"] = std::string(s);
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		ImGui::SetNextItemWidth(10 * ImGui::GetFontSize());
		if(ImGui::BeginCombo("on", trigger_operator.value.c_str()))
		{
			for(const char* o : {"raw", "magnitude"})
			{
				if(ImGui::Selectable(o, trigger_operator.value == o))
					staged["trigger_operator"] = std::string(o);
			}
			ImGui::EndCombo();
		}

		//Per trace: visibility, colour, and which one the vertical controls act on
		for(std::size_t i = 0; i < ntraces; i++)
		{
			if(i > 0)
				ImGui::SameLine();

			ImGui::PushID(static_cast<int>(i));

			bool visible = _area->GetTraceVisible(i);
			ImGui::PushStyleColor(ImGuiCol_Text, _area->GetTraceColor(i));
			if(ImGui::Checkbox(_area->GetTraceName(i), &visible))
			{
				_area->SetTraceVisible(i, visible);
				staged["trace_mask"] = static_cast<std::uint32_t>(_area->GetTraceMask());
			}
			ImGui::PopStyleColor();

			ImGui::PopID();
		}

		if(ntraces > 0)
		{
			//Which trace the vertical controls act on, as a visible control rather than a hidden
			//gesture. "all" is the default and is what makes the V/div box behave the way a bench
			//scope's does on an overlaid plot; picking a single trace is for when two channels
			//have genuinely different amplitudes.
			const std::size_t sel = _area->GetSelectedTrace();
			const bool ganged = _area->GetGanged();

			ImGui::SetNextItemWidth(10 * ImGui::GetFontSize());
			if(ImGui::BeginCombo("channel", ganged ? "all" : _area->GetTraceName(sel)))
			{
				if(ImGui::Selectable("all", ganged))
					_area->SetGanged(true);

				for(std::size_t i = 0; i < ntraces; i++)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, _area->GetTraceColor(i));
					if(ImGui::Selectable(_area->GetTraceName(i), !ganged && (i == sel)))
					{
						_area->SetGanged(false);
						_area->SetSelectedTrace(i);
					}
					ImGui::PopStyleColor();
				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			ImGui::SetNextItemWidth(12 * ImGui::GetFontSize());
			float vpd = _area->GetVoltsPerDiv(ganged ? 0 : sel);
			if(ImGui::DragFloat("V/div", &vpd, 0.01f, 1e-6f, 1e6f, "%.4g",
				ImGuiSliderFlags_Logarithmic))
			{
				_area->ApplyVoltsPerDiv(vpd);
				staged["volts_per_div"] = vpd;
			}

			ImGui::SameLine();
			ImGui::TextDisabled("(ctrl+drag moves, ctrl+wheel scales)");
		}

		detail::RenderStreamStatus(_status, _accepted.load(std::memory_order_relaxed),
			_dropped.load(std::memory_order_relaxed),
			_rateConflict ? "ports disagree about the sample rate" : nullptr);

		if(_capture->GetResyncCount() > 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "| %" PRIu64 " resyncs",
				static_cast<std::uint64_t>(_capture->GetResyncCount()));
		}

		if(!staged.empty())
			std::ignore = this->settings().setStaged(std::move(staged));
	}

public:
	////////////////////////////////////////////////////////////////////////////////////////////////
	// Enum spellings
	//
	// An unrecognised name is a typo in a flowgraph, not a reason to refuse to display anything,
	// so every one of these falls back to the default rather than throwing. Public because that
	// fallback is part of the block's contract with a flowgraph, and so is worth testing.

	static TriggerMode ModeFromName(const std::string& name)
	{
		if(name == "stop")
			return TriggerMode::Stop;
		if(name == "normal")
			return TriggerMode::Normal;
		if(name == "single")
			return TriggerMode::Single;
		return TriggerMode::Auto;
	}

	static TriggerKind KindFromName(const std::string& name)
	{
		if((name == "free") || (name == "freerun") || (name == "free-run"))
			return TriggerKind::FreeRun;
		return TriggerKind::Edge;
	}

	static TriggerSlope SlopeFromName(const std::string& name)
	{
		if(name == "falling")
			return TriggerSlope::Falling;
		if(name == "any")
			return TriggerSlope::Any;
		return TriggerSlope::Rising;
	}

	static TriggerOperator OperatorFromName(const std::string& name)
	{
		if((name == "magnitude") || (name == "mag") || (name == "envelope"))
			return TriggerOperator::Magnitude;
		return TriggerOperator::Raw;
	}
};

} // namespace gr::imcufosphor

#endif
