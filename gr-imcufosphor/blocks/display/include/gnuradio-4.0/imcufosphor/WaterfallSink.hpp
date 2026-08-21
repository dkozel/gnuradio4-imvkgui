/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Declaration of WaterfallSink
 */
#ifndef GR_IMCUFOSPHOR_WATERFALL_SINK_HPP
#define GR_IMCUFOSPHOR_WATERFALL_SINK_HPP

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <gnuradio-4.0/imcufosphor/detail/IqRing.hpp>
#include <gnuradio-4.0/imcufosphor/detail/MetaFromTags.hpp>
#include <gnuradio-4.0/imcufosphor/detail/StreamStatus.hpp>

#include "RenderHost.h"
#include "SpectrumEngine.h"
#include "WaterfallArea.h"

namespace gr::imcufosphor {

using namespace gr;

GR_REGISTER_BLOCK("gr::imcufosphor::WaterfallSink", gr::imcufosphor::WaterfallSink, ([T]),
	[ std::complex<float>, std::complex<std::int16_t> ])

/**
	@brief Scrolling spectrogram display

	The waterfall half of AnalyzerSink on its own. Built with only the Waterfall part of the
	engine, so it allocates no hit histogram - which at 8192 bins by 1024 amplitude cells is the
	largest buffer in the pipeline and is pure waste to a display that never draws a density map.

	@par group_size is the control that matters here

	The transform rate is fixed by the sample rate and fft_size, and is normally far higher than
	any useful line rate: 245.76 MS/s at 8192 points is 30000 spectra per second against a
	display that can show perhaps 60 rows. group_size sets how many spectra fold into one row,
	which is what keeps the resolution bandwidth, the line rate and the playback speed
	independent of each other rather than tied together.

	See AnalyzerSink for the threading contract, the no-backpressure policy and the
	centre-frequency tag convention, all of which are identical here.
 */
template<typename T>
requires std::same_as<T, std::complex<float>> || std::same_as<T, std::complex<std::int16_t>>
struct WaterfallSink : Block<WaterfallSink<T>, Drawable<UICategory::Content, "ImGui">>
{
	using Description = Doc<R""(@brief Vulkan scrolling spectrogram display.

Consumes complex IQ and renders a waterfall. group_size sets how many spectra are folded into
each row, decoupling the line rate from the transform rate. Centre frequency comes from the
'frequency' tag, falling back to trigger_meta_info["core:frequency"] and then to the
center_frequency setting.

Allocates no density histogram; use AnalyzerSink if both panes are wanted, so that one
transform feeds both. Requires a host application to have published an imcufosphor RenderHost;
without one it runs headless, consuming and dropping samples.)"">;

	template<typename U, gr::meta::fixed_string TDescription = "", typename... Arguments>
	using A = Annotated<U, TDescription, Arguments...>;

	PortIn<T> in;

	A<gr::Size_t, "fft_size", Visible, Doc<"Transform length, which sets the resolution bandwidth">>
		fft_size = 8192U;

	A<gr::Size_t, "block_size", Visible, Doc<"IQ samples per GPU submit">>
		block_size = 1048576U;

	A<gr::Size_t, "group_size", Visible, Doc<"Spectra folded into each waterfall row">>
		group_size = 1U;

	A<float, "sample_rate", Visible, Unit<"Hz">, Doc<"Fallback sample rate, used until a tag supplies one">>
		sample_rate = 1.0f;

	A<double, "center_frequency", Visible, Unit<"Hz">, Doc<"Fallback centre frequency">>
		center_frequency = 0.0;

	A<bool, "ignore_tag_sample_rate", Doc<"Use the sample_rate setting even when the stream tags one">>
		ignore_tag_sample_rate = false;

	A<float, "sample_scale", Doc<"Raw LSB to volts for integer input">>
		sample_scale = 1.0f / 32767.0f;

	A<std::string, "window", Visible, Doc<"blackman-harris | hamming | hann | rectangular">>
		window = "blackman-harris";

	A<float, "db_min", Visible, Unit<"dBm">, Doc<"Bottom of the colour scale">>
		db_min = -100.0f;

	A<float, "db_max", Visible, Unit<"dBm">, Doc<"Top of the colour scale">>
		db_max = -20.0f;

	A<std::string, "color_map", Doc<"Colour ramp name, resolved by the host's TextureManager">>
		color_map = "eye-gradient-viridis";

	A<std::uint64_t, "timeout_ms", Unit<"ms">, Doc<"Minimum interval between draws. 0 draws every frame">>
		timeout_ms = 33ULL;

	A<gr::Size_t, "buffer_depth", Doc<"Handoff ring capacity in samples. 0 means four blocks">>
		buffer_depth = 0U;

	GR_MAKE_REFLECTABLE(WaterfallSink, in, fft_size, block_size, group_size, sample_rate,
		center_frequency, ignore_tag_sample_rate, sample_scale, window, db_min, db_max,
		color_map, timeout_ms, buffer_depth);

	//Thread ownership is exactly as documented on AnalyzerSink.
	detail::IqRing<T> _ring;

	std::atomic<double> _metaRate{0.0};
	std::atomic<double> _metaCenter{0.0};
	std::atomic<bool> _metaDirty{false};
	std::atomic<bool> _reconfigure{true};

	///@brief What the stream has reported about itself. See detail/StreamStatus.hpp.
	detail::StreamStatus _status;

	std::unique_ptr<SpectrumEngine> _engine;
	std::unique_ptr<WaterfallArea> _area;
	bool _initialised = false;
	std::chrono::steady_clock::time_point _lastDraw{};

	void start()
	{
		const std::size_t depth = (buffer_depth > 0U)
			? static_cast<std::size_t>(buffer_depth)
			: 4UZ * static_cast<std::size_t>(block_size);

		_ring.Resize(depth);
		_reconfigure.store(true, std::memory_order_release);

		if(!::imcufosphor::globalRenderHost().Available())
		{
			LogNotice("WaterfallSink '%s': no RenderHost published, running headless "
				"(samples will be consumed and dropped)\n", this->name.value.c_str());
		}
	}

	///@brief Nothing: GPU teardown belongs to the render thread. See AnalyzerSink::stop().
	void stop() {}

	///@brief Drops the GPU resources. Render thread only, after the scheduler has been joined.
	void ReleaseGpuResources()
	{
		_area.reset();
		_engine.reset();
		_initialised = false;
	}

	void settingsChanged(const property_map& /*oldSettings*/, const property_map& /*newSettings*/)
	{
		_reconfigure.store(true, std::memory_order_release);
	}

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

		_ring.Push(std::span<const T>(dataIn.data(), n));

		std::ignore = dataIn.consume(n);
		return work::Status::OK;
	}

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
			//Waterfall only: no SpectrumDensity, and therefore no hit histogram
			_engine = std::make_unique<SpectrumEngine>(EnginePart::Waterfall, host.RenderQueue());
			_area = std::make_unique<WaterfallArea>(
				_engine->GetWaterfall(), host.Textures(), color_map);
			_area->SetShowXAxis(true);

			//No RecordingClock: a live stream has no wall clock to pin rows to, which
			//WaterfallArea handles by dropping the timestamp from its hover readout and
			//keeping the age axis, which the row history alone is enough for.
			_area->SetTimebase(&_engine->GetRowHistory(), nullptr, _engine->GetSampleRate());

			_initialised = true;
			_reconfigure.store(true, std::memory_order_release);
		}

		const bool metaChanged = _metaDirty.exchange(false, std::memory_order_acq_rel);
		if(_reconfigure.exchange(false, std::memory_order_acq_rel) || metaChanged)
			ApplyConfiguration();

		//timeout_ms rate-limits the GPU work, and nothing else.
		//
		//It must not skip the ImGui calls below. ImGui is immediate mode: a window that is not
		//submitted on a frame is not drawn on that frame, so returning early here blanks the
		//whole pane until the next draw that gets through - which at a 33 ms limit against a
		//16.7 ms vsync is every other frame, i.e. a 30 Hz flicker. Skipping the compute is
		//free by comparison: the texture still holds what the last tone map wrote.
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
			//Frozen after end_of_stream: keep drawing the last state, stop advancing it
			if(!_status.Frozen())
			{
				_ring.WithBlock(static_cast<std::size_t>(block_size),
					[this](std::span<const T> samples) { _engine->Step(samples); });
			}

			if(auto* cmdBuf = host.FrameComputeCommandBuffer())
			{
				cmdBuf->begin({});
				_area->ToneMap(*cmdBuf);
				cmdBuf->end();
				host.RenderQueue()->SubmitAndBlock(*cmdBuf);
			}
		}

		ImGui::Begin(this->name.value.c_str());
		detail::RenderStreamStatus(_status, _ring.Pushed(), _ring.Dropped(), nullptr);
		const auto avail = ImGui::GetContentRegionAvail();
		if((avail.x > 0) && (avail.y > 0))
			_area->Render(avail);
		ImGui::End();

		return status();
	}

protected:
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

		const double taggedRate = _metaRate.load(std::memory_order_relaxed);
		cfg.sampleRate = (taggedRate > 0) ? taggedRate : static_cast<double>(sample_rate);

		const double taggedCenter = _metaCenter.load(std::memory_order_relaxed);
		cfg.centerFrequency = (taggedCenter != 0.0) ? taggedCenter : center_frequency.value;

		_engine->Configure(cfg);

		//Re-pushed because the age axis is derived from it, and a retune or a rate change
		//makes the previous value wrong
		_area->SetTimebase(&_engine->GetRowHistory(), nullptr, _engine->GetSampleRate());
	}

	static FFTFilter::WindowFunction WindowFromName(const std::string& name)
	{
		if(name == "rectangular")
			return FFTFilter::WINDOW_RECTANGULAR;
		if(name == "hamming")
			return FFTFilter::WINDOW_HAMMING;
		if(name == "hann")
			return FFTFilter::WINDOW_HANN;
		return FFTFilter::WINDOW_BLACKMAN_HARRIS;
	}
};

} // namespace gr::imcufosphor

#endif
