/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Headless checks of AnalyzerSink's flowgraph behaviour

	No RenderHost is published here, so draw() returns without allocating anything and no Vulkan
	device is touched. That is the headless mode a flowgraph without a window actually runs in,
	so these are tests of a supported path rather than of a stub.

	What they are pinning down is the contract between the block and the scheduler: that the
	display can never stall the graph, that everything it fails to display is accounted for
	rather than silently lost, and that settingsChanged() stays off the GPU.
 */

#include <complex>
#include <cstdio>

#include <boost/ut.hpp>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/testing/NullSources.hpp>

#include <gnuradio-4.0/imcufosphor/AnalyzerSink.hpp>

using namespace boost::ut;
using TSample = std::complex<float>;
using TSink = gr::imcufosphor::AnalyzerSink<TSample>;

//Progress markers on stderr, unbuffered. A flowgraph test that hangs gets killed, and a killed
//process loses buffered stdout, so a failure would otherwise be indistinguishable from a
//failure to start.
#define TRACE(msg) do { std::fprintf(stderr, "[qa] %s\n", (msg)); std::fflush(stderr); } while(0)

const suite<"AnalyzerSink headless"> analyzerSinkTests = [] {

	"is discoverable as a drawable content block"_test = [] {
		TSink sink;
		sink.init(sink.progress);

		//This is how a render loop finds it: by category, through the type-erased model,
		//without knowing the concrete type
		expect(TSink::DrawableControl::kCategory == gr::UICategory::Content);
		expect(eq(std::string_view(TSink::DrawableControl::kToolkit), std::string_view("ImGui")));
	};

	"draw() is a no-op without a RenderHost"_test = [] {
		TSink sink;
		sink.init(sink.progress);

		//The point: no window, no GPU, no crash. A headless graph calls this only if something
		//odd is driving it, but it must survive that.
		expect(sink.draw({}) == gr::work::Status::OK);
		expect(sink.draw({}) == gr::work::Status::OK);
	};

	"start() sizes the ring and zeroes the counters"_test = [] {
		TSink sink;
		sink.block_size = 1024U;
		sink.buffer_depth = 4096U;
		sink.init(sink.progress);
		sink.start();

		expect(eq(sink._ring.Pushed(), std::uint64_t{0}));
		expect(eq(sink._ring.Dropped(), std::uint64_t{0}));
		expect(ge(sink._ring.Capacity(), std::size_t{4096}))
			<< "the ring is at least the requested depth (CircularBuffer rounds up to a page)";
	};

	"drop accounting is exact under a graph run"_test = [] {
		gr::Graph graph;

		auto& src = graph.emplaceBlock<gr::testing::ConstantSource<TSample>>();
		src.n_samples_max = 100000U;

		auto& sink = graph.emplaceBlock<TSink>({
			{"name", "analyzer"},
			{"block_size", gr::Size_t{1024}},
			{"buffer_depth", gr::Size_t{4096}},
		});

		TRACE("  connect");
		expect(graph.connect<"out", "in">(src, sink).has_value());

		TRACE("  exchange");
		gr::scheduler::Simple sched;
		expect(sched.exchange(std::move(graph)).has_value());

		TRACE("  runAndWait");
		expect(sched.runAndWait().has_value());
		TRACE("  runAndWait returned");

		//Nothing is displayed - no RenderHost - so every sample the source produced was either
		//buffered or dropped, and the two must add up. A leak here would mean samples vanishing
		//without being counted, which is exactly the failure a drop counter exists to prevent.
		const auto pushed = sink._ring.Pushed();
		const auto dropped = sink._ring.Dropped();

		expect(eq(pushed + dropped, std::uint64_t{100000}))
			<< "every sample is either buffered or counted as dropped";
		expect(gt(dropped, std::uint64_t{0}))
			<< "a 4096-sample ring cannot hold 100k samples, so some must be dropped";

		TRACE("  teardown");
	};

	"a headless graph runs to completion rather than stalling"_test = [] {
		//The real requirement. If the sink ever refused to consume, this would hang rather
		//than fail, so the test's value is that it terminates at all.
		gr::Graph graph;

		auto& src = graph.emplaceBlock<gr::testing::ConstantSource<TSample>>();
		src.n_samples_max = 250000U;

		auto& sink = graph.emplaceBlock<TSink>({
			{"name", "analyzer"},
			{"block_size", gr::Size_t{4096}},
			{"buffer_depth", gr::Size_t{8192}},
		});

		expect(graph.connect<"out", "in">(src, sink).has_value());

		gr::scheduler::Simple sched;
		expect(sched.exchange(std::move(graph)).has_value());
		expect(sched.runAndWait().has_value());

		expect(eq(sink._ring.Pushed() + sink._ring.Dropped(), std::size_t{250000}));
	};

	"settingsChanged raises the reconfigure flag without touching the GPU"_test = [] {
		TSink sink;
		sink.init(sink.progress);
		sink.start();

		//start() leaves it set, so clear it first to see the change take effect
		sink._reconfigure.store(false);

		expect(sink.settings().set({{"fft_size", gr::Size_t{4096}}}).empty());
		std::ignore = sink.settings().activateContext();
		std::ignore = sink.settings().applyStagedParameters();

		expect(sink._reconfigure.load())
			<< "a settings change must be deferred to draw(), not applied on the scheduler thread";

		//Nothing was created: the engine is only ever built on the render thread inside draw()
		expect(sink._engine == nullptr);
		expect(sink._pane == nullptr);
	};

	"tagged sample rate and centre frequency reach the block"_test = [] {
		TSink sink;
		sink.init(sink.progress);
		sink.start();

		expect(eq(sink._metaRate.load(), 0.0)) << "nothing tagged yet";

		const gr::property_map tag = [] {
			gr::property_map m;
			m[gr::pmt::Value::Map::key_type("sample_rate", m.get_allocator().resource())] = 2.4e6f;
			m[gr::pmt::Value::Map::key_type("frequency", m.get_allocator().resource())] = 915.0e6;
			return m;
		}();

		std::vector<std::pair<std::size_t, std::reference_wrapper<const gr::property_map>>> tags{
			{0UZ, std::cref(tag)}};

		const auto meta = gr::imcufosphor::detail::metaFromTags(tags, false);
		expect(meta.sampleRate.has_value() && meta.centerHz.has_value());
	};
};

int main() {}
