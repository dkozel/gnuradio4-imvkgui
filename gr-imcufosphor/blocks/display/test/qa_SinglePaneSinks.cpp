/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Headless checks of SpectrumSink and WaterfallSink

	The same contract qa_AnalyzerSink pins down, applied to the two single-pane sinks: they are
	discoverable as drawable content, they never stall a graph, and they account for every
	sample they cannot display.

	Headless by construction - no RenderHost is published - so no GPU is touched. What that
	cannot cover is the part that differs between these two and AnalyzerSink, which is which
	half of the engine gets built; that is checked on the GPU by
	wfbench --verify's SpectrumEngine parts-mask test.
 */

#include <complex>

#include <boost/ut.hpp>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/testing/NullSources.hpp>

#include <gnuradio-4.0/imcufosphor/SpectrumSink.hpp>
#include <gnuradio-4.0/imcufosphor/WaterfallSink.hpp>

using namespace boost::ut;
using TSample = std::complex<float>;

namespace {

///@brief Runs a fixed number of samples through a sink and checks nothing is lost or stalls
template<typename TSink>
void CheckGraphRun(std::uint32_t nsamples)
{
	gr::Graph graph;

	auto& src = graph.emplaceBlock<gr::testing::ConstantSource<TSample>>();
	src.n_samples_max = nsamples;

	auto& sink = graph.emplaceBlock<TSink>({
		{"name", "display"},
		{"block_size", gr::Size_t{1024}},
		{"buffer_depth", gr::Size_t{4096}},
	});

	expect(graph.connect<"out", "in">(src, sink).has_value());

	gr::scheduler::Simple sched;
	expect(sched.exchange(std::move(graph)).has_value());
	expect(sched.runAndWait().has_value());

	expect(eq(sink._ring.Pushed() + sink._ring.Dropped(), std::uint64_t{nsamples}))
		<< "every sample is either buffered or counted as dropped";

	//Nothing was built on the GPU: without a RenderHost, draw() returns before allocating
	expect(sink._engine == nullptr);
	expect(sink._area == nullptr);
}

} // namespace

const suite<"SpectrumSink headless"> spectrumSinkTests = [] {

	"is drawable content"_test = [] {
		using TSink = gr::imcufosphor::SpectrumSink<TSample>;
		expect(TSink::DrawableControl::kCategory == gr::UICategory::Content);
		expect(eq(std::string_view(TSink::DrawableControl::kToolkit), std::string_view("ImGui")));
	};

	"draw() is a no-op without a RenderHost"_test = [] {
		gr::imcufosphor::SpectrumSink<TSample> sink;
		sink.init(sink.progress);
		expect(sink.draw({}) == gr::work::Status::OK);
	};

	"runs a graph to completion with exact drop accounting"_test = [] {
		CheckGraphRun<gr::imcufosphor::SpectrumSink<TSample>>(100000U);
	};

	"trace_mask defaults to all five traces"_test = [] {
		gr::imcufosphor::SpectrumSink<TSample> sink;
		sink.init(sink.progress);

		//0x1f is mean, median, low, mid, high. A narrower default would silently hide traces
		//that the density map alone cannot substitute for.
		expect(eq(sink.trace_mask.value, std::uint32_t{0x1f}));
		expect(eq(std::size_t{5}, SpectrumArea::NUM_TRACES));
	};
};

const suite<"WaterfallSink headless"> waterfallSinkTests = [] {

	"is drawable content"_test = [] {
		using TSink = gr::imcufosphor::WaterfallSink<TSample>;
		expect(TSink::DrawableControl::kCategory == gr::UICategory::Content);
		expect(eq(std::string_view(TSink::DrawableControl::kToolkit), std::string_view("ImGui")));
	};

	"draw() is a no-op without a RenderHost"_test = [] {
		gr::imcufosphor::WaterfallSink<TSample> sink;
		sink.init(sink.progress);
		expect(sink.draw({}) == gr::work::Status::OK);
	};

	"runs a graph to completion with exact drop accounting"_test = [] {
		CheckGraphRun<gr::imcufosphor::WaterfallSink<TSample>>(100000U);
	};

	"group_size defaults to one spectrum per row"_test = [] {
		gr::imcufosphor::WaterfallSink<TSample> sink;
		sink.init(sink.progress);
		expect(eq(sink.group_size.value, gr::Size_t{1}));
	};
};

const suite<"sink instantiations"> instantiationTests = [] {

	"both sample types instantiate"_test = [] {
		//The ci16 instantiation exists so a native SDR stream reaches the GPU without a host
		//side conversion. It has to keep compiling even though nothing in this file exercises
		//it at runtime.
		gr::imcufosphor::SpectrumSink<std::complex<std::int16_t>> s;
		gr::imcufosphor::WaterfallSink<std::complex<std::int16_t>> w;
		s.init(s.progress);
		w.init(w.progress);
		expect(true);
	};
};

int main() {}
