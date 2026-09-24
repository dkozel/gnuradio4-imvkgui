/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Headless checks of ScopeSink's flowgraph behaviour

	No RenderHost is published, so draw() returns without allocating and no Vulkan device is
	touched. See qa_AnalyzerSink.cpp for why that is a test of a supported path rather than of a
	stub.

	What is specific to this block is the variable port count. Two things about it can only be
	checked here: that the ports are sized before anything connects, and that refusing to resize
	them afterwards actually happens - the type-erased port table holds non-owning references
	into the vector, so a late resize would dangle every one of them rather than fail visibly.
 */

#include <complex>
#include <cstdio>
#include <string>

#include <boost/ut.hpp>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/testing/NullSources.hpp>

#include <gnuradio-4.0/imcufosphor/ScopeSink.hpp>

using namespace boost::ut;
using TComplex = std::complex<float>;
using TComplexSink = gr::imcufosphor::ScopeSink<TComplex>;
using TRealSink = gr::imcufosphor::ScopeSink<float>;

//Progress markers on stderr, unbuffered: a flowgraph test that hangs gets killed, and a killed
//process loses buffered stdout, so a hang would otherwise look like a failure to start.
#define TRACE(msg) do { std::fprintf(stderr, "[qa] %s\n", (msg)); std::fflush(stderr); } while(0)

const suite<"ScopeSink headless"> scopeSinkTests = [] {

	"is discoverable as a drawable content block"_test = [] {
		TComplexSink sink;
		sink.init(sink.progress);

		expect(TComplexSink::DrawableControl::kCategory == gr::UICategory::Content);
		expect(eq(std::string_view(TComplexSink::DrawableControl::kToolkit), std::string_view("ImGui")));
	};

	"draw() is a no-op without a RenderHost"_test = [] {
		TComplexSink sink;
		sink.init(sink.progress);

		expect(sink.draw({}) == gr::work::Status::OK);
		expect(sink.draw({}) == gr::work::Status::OK);
	};

	"a complex port is two components, a real port is one"_test = [] {
		//This is what the reinterpret_cast in Drain() rests on, so it is worth stating out loud
		//rather than leaving to the static_assert alone.
		expect(eq(TComplexSink::kComponents, std::size_t{2}));
		expect(eq(TRealSink::kComponents, std::size_t{1}));
	};

	"n_inputs sizes the port collection at construction"_test = [] {
		gr::Graph graph;
		auto& sink = graph.emplaceBlock<TComplexSink>({
			{"name", "scope"},
			{"n_inputs", gr::Size_t{3}},
		});

		//init() applies the staged settings, so this has already happened by the time
		//emplaceBlock returns - which is what lets a caller connect to "in#2" straight away.
		expect(eq(sink.in.size(), std::size_t{3}));
	};

	"n_inputs is construction-time only"_test = [] {
		//The guard exists because BlockWrapper's port table takes non-owning references into the
		//vector (BlockModel.hpp:673-700). Resizing after those exist dangles every one of them,
		//and the use-after-free is far worse than the exception.
		//
		//Note what this does NOT test: a port reporting isConnected(). That was the obvious guard
		//and it does not work - Port::isConnected() for an input means "the buffer has a writer",
		//which this gnuradio4 only makes true once the scheduler runs. Measured: false after
		//connect(), false after exchange(), true only after runAndWait(). By exchange() the
		//references already exist, so a guard keyed on it would open after the window it was
		//meant to protect. Latching on the first settings application cannot be late.
		gr::Graph graph;

		auto& sink = graph.emplaceBlock<TComplexSink>({
			{"name", "scope"},
			{"n_inputs", gr::Size_t{2}},
		});
		expect(eq(sink.in.size(), std::size_t{2})) << "the construction-time value is honoured";

		bool threw = false;
		try
		{
			std::ignore = sink.settings().set({{"n_inputs", gr::Size_t{4}}});
			std::ignore = sink.settings().activateContext();
			std::ignore = sink.settings().applyStagedParameters();
		}
		catch(const std::exception&)
		{
			threw = true;
		}

		expect(threw) << "a later change must be refused, not silently applied";
		expect(eq(sink.in.size(), std::size_t{2}));
	};

	"setting n_inputs to what it already is is not an error"_test = [] {
		//Settings get reapplied for all sorts of reasons. Only an actual change is a problem, and
		//throwing on a no-op would make the block impossible to reconfigure at all.
		gr::Graph graph;
		auto& sink = graph.emplaceBlock<TComplexSink>({
			{"name", "scope"},
			{"n_inputs", gr::Size_t{2}},
		});

		bool threw = false;
		try
		{
			std::ignore = sink.settings().set({
				{"n_inputs", gr::Size_t{2}},
				{"record_length", gr::Size_t{2048}},
			});
			std::ignore = sink.settings().activateContext();
			std::ignore = sink.settings().applyStagedParameters();
		}
		catch(const std::exception&)
		{
			threw = true;
		}

		expect(!threw);
		expect(eq(sink.in.size(), std::size_t{2}));
		expect(eq(static_cast<std::size_t>(sink.record_length), std::size_t{2048}))
			<< "and the other settings in the same batch still applied";
	};

	"start() sizes one ring per port"_test = [] {
		TComplexSink sink;
		sink.n_inputs = 2U;
		sink.record_length = 1024U;
		sink.buffer_depth = 4096U;
		sink.init(sink.progress);
		sink.start();

		expect(eq(sink._ports.size(), std::size_t{2}));
		for(const auto& p : sink._ports)
		{
			expect(ge(p->ring.Capacity(), std::size_t{4096}))
				<< "at least the requested depth (CircularBuffer rounds up to a page)";
			expect(eq(p->ring.Dropped(), std::uint64_t{0}));
		}
	};

	"settingsChanged stays off the GPU"_test = [] {
		TComplexSink sink;
		sink.init(sink.progress);
		sink.start();

		//No RenderHost, no device. If settingsChanged ever reached for one this would crash
		//rather than fail, which is the point.
		sink._reconfigure.store(false);
		sink.settingsChanged({}, {{"record_length", gr::Size_t{4096}}});
		expect(sink._reconfigure.load());
	};

	"a single-port graph runs to completion rather than stalling"_test = [] {
		//The real requirement. A sink that refused to consume would hang here rather than fail,
		//so the value of this test is that it terminates at all.
		gr::Graph graph;

		auto& src = graph.emplaceBlock<gr::testing::ConstantSource<TComplex>>();
		src.n_samples_max = 50000U;

		auto& sink = graph.emplaceBlock<TComplexSink>({
			{"name", "scope"},
			{"n_inputs", gr::Size_t{1}},
			{"record_length", gr::Size_t{1024}},
			{"buffer_depth", gr::Size_t{4096}},
		});

		TRACE("  connect");
		expect(graph.connect(src, "out", sink, "in#0").has_value());

		TRACE("  exchange");
		gr::scheduler::Simple sched;
		expect(sched.exchange(std::move(graph)).has_value());

		TRACE("  runAndWait");
		expect(sched.runAndWait().has_value());
		TRACE("  runAndWait returned");

		//Nothing is displayed, so every sample was either accepted into a ring or counted as
		//dropped. A leak here would be samples vanishing unaccounted for, which is exactly what
		//the drop counter exists to prevent.
		const auto accepted = sink._accepted.load();
		const auto dropped = sink._dropped.load();

		expect(eq(accepted + dropped, std::uint64_t{50000}))
			<< "every sample is either accepted or counted";
		expect(gt(dropped, std::uint64_t{0}))
			<< "a 4096-sample ring cannot hold 50k samples";
		expect(eq(sink._ports[0]->ring.Dropped(), std::uint64_t{0}))
			<< "PushUpTo never drops inside the ring; the block owns the count";

		TRACE("  teardown");
	};

	"two ports stay in step under joint admission"_test = [] {
		//The invariant ScopeCapture asserts. Both rings must accept exactly the same count on
		//every call, or the two channels drift apart in time and the display lies about which
		//sample lines up with which.
		gr::Graph graph;

		auto& src0 = graph.emplaceBlock<gr::testing::ConstantSource<TComplex>>();
		src0.n_samples_max = 40000U;
		auto& src1 = graph.emplaceBlock<gr::testing::ConstantSource<TComplex>>();
		src1.n_samples_max = 40000U;

		auto& sink = graph.emplaceBlock<TComplexSink>({
			{"name", "scope2"},
			{"n_inputs", gr::Size_t{2}},
			{"record_length", gr::Size_t{512}},
			{"buffer_depth", gr::Size_t{4096}},
		});

		TRACE("  connect 2ch");
		expect(graph.connect(src0, "out", sink, "in#0").has_value());
		expect(graph.connect(src1, "out", sink, "in#1").has_value());

		gr::scheduler::Simple sched;
		expect(sched.exchange(std::move(graph)).has_value());

		TRACE("  runAndWait 2ch");
		expect(sched.runAndWait().has_value());
		TRACE("  runAndWait 2ch returned");

		const auto a0 = sink._ports[0]->ring.Pushed();
		const auto a1 = sink._ports[1]->ring.Pushed();
		expect(eq(a0, a1)) << "both rings accepted the same number of samples";
		expect(eq(a0, sink._accepted.load()));

		//And nothing was lost inside a ring where the caller could not see how much
		expect(eq(sink._ports[0]->ring.Dropped(), std::uint64_t{0}))
			<< "PushUpTo never drops; shortfalls are counted by the block, not the ring";
		expect(eq(sink._ports[1]->ring.Dropped(), std::uint64_t{0}));

		TRACE("  teardown 2ch");
	};

	"a real-valued graph runs"_test = [] {
		//The float instantiation, which is the first real-input block in this project.
		gr::Graph graph;

		auto& src = graph.emplaceBlock<gr::testing::ConstantSource<float>>();
		src.n_samples_max = 20000U;

		auto& sink = graph.emplaceBlock<TRealSink>({
			{"name", "scope_real"},
			{"n_inputs", gr::Size_t{1}},
			{"record_length", gr::Size_t{512}},
			{"buffer_depth", gr::Size_t{4096}},
		});

		expect(graph.connect(src, "out", sink, "in#0").has_value());

		gr::scheduler::Simple sched;
		expect(sched.exchange(std::move(graph)).has_value());
		expect(sched.runAndWait().has_value());

		expect(eq(sink._accepted.load() + sink._dropped.load(), std::uint64_t{20000}));
	};

	"a discontinuity is recorded when a ring refuses samples"_test = [] {
		gr::Graph graph;

		auto& src = graph.emplaceBlock<gr::testing::ConstantSource<TComplex>>();
		src.n_samples_max = 50000U;

		auto& sink = graph.emplaceBlock<TComplexSink>({
			{"name", "scope"},
			{"n_inputs", gr::Size_t{1}},
			{"record_length", gr::Size_t{256}},
			{"buffer_depth", gr::Size_t{4096}},
		});

		expect(graph.connect(src, "out", sink, "in#0").has_value());

		gr::scheduler::Simple sched;
		expect(sched.exchange(std::move(graph)).has_value());
		expect(sched.runAndWait().has_value());

		//Samples were certainly refused at this ring depth, and every refusal means the stream is
		//no longer contiguous from there on. A record spanning that point must never be drawn as
		//if it were, so the marker has to exist.
		expect(gt(sink._discontinuity.load(), std::uint64_t{0}));
		expect(le(sink._discontinuity.load(), sink._accepted.load()));
	};

	"enum spellings fall back rather than refusing to draw"_test = [] {
		//An unrecognised name is a typo in a flowgraph, not a reason to show nothing.
		expect(TComplexSink::ModeFromName("normal") == TriggerMode::Normal);
		expect(TComplexSink::ModeFromName("nonsense") == TriggerMode::Auto);
		expect(TComplexSink::KindFromName("free") == TriggerKind::FreeRun);
		expect(TComplexSink::KindFromName("") == TriggerKind::Edge);
		expect(TComplexSink::SlopeFromName("falling") == TriggerSlope::Falling);
		expect(TComplexSink::SlopeFromName("sideways") == TriggerSlope::Rising);
		expect(TComplexSink::OperatorFromName("magnitude") == TriggerOperator::Magnitude);
		expect(TComplexSink::OperatorFromName("logarithm") == TriggerOperator::Raw);
	};
};

int main() {}
