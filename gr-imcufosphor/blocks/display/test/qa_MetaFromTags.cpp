/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Checks sample rate and centre frequency extraction from tags

	Pure header logic, no GPU and no flowgraph. The precedence order is the part worth pinning
	down: a display that silently preferred the wrong source would draw a correct-looking
	spectrum with a wrong frequency axis, which is worse than not drawing one.
 */

#include <boost/ut.hpp>

#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/imcufosphor/detail/MetaFromTags.hpp>

using namespace boost::ut;
using namespace gr::imcufosphor::detail;

namespace {

///@brief Builds a property_map, working around the pmr key type
gr::property_map makeTag(std::initializer_list<std::pair<std::string_view, gr::pmt::Value>> entries)
{
	gr::property_map map;
	for(const auto& [k, v] : entries)
		map[gr::pmt::Value::Map::key_type(std::string(k), map.get_allocator().resource())] = v;
	return map;
}

} // namespace

const suite<"tag metadata extraction"> metaTests = [] {

	"reads the sample_rate and frequency default tags"_test = [] {
		const auto tag = makeTag({{"sample_rate", 2.4e6f}, {"frequency", 915.0e6}});
		const auto meta = metaFromTag(tag, false);

		expect(meta.sampleRate.has_value());
		expect(eq(*meta.sampleRate, 2.4e6));
		expect(meta.centerHz.has_value());
		expect(eq(*meta.centerHz, 915.0e6));
	};

	"coerces every arithmetic alternative"_test = [] {
		//gr::tag::FREQUENCY is declared double and SAMPLE_RATE float, but a block filling a
		//property_map by hand can produce any of these
		expect(eq(*metaFromTag(makeTag({{"frequency", 1.0e9}}), false).centerHz, 1.0e9));
		expect(eq(*metaFromTag(makeTag({{"frequency", 1.0e9f}}), false).centerHz, 1.0e9));
		expect(eq(*metaFromTag(makeTag({{"frequency", std::uint64_t{1000000000}}}), false).centerHz, 1.0e9));
		expect(eq(*metaFromTag(makeTag({{"frequency", std::int64_t{1000000000}}}), false).centerHz, 1.0e9));
	};

	"a non-numeric value is treated as absent"_test = [] {
		const auto meta = metaFromTag(makeTag({{"frequency", std::string("nine hundred megahertz")}}), false);
		expect(!meta.centerHz.has_value());
	};

	"a non-positive sample rate is treated as absent"_test = [] {
		//Zero would be a divide by zero in the RBW and the decay constants; negative is
		//meaningless. Both must fall back to the setting rather than propagate.
		expect(!metaFromTag(makeTag({{"sample_rate", 0.0f}}), false).sampleRate.has_value());
		expect(!metaFromTag(makeTag({{"sample_rate", -1.0f}}), false).sampleRate.has_value());
	};

	"ignore_tag_sample_rate suppresses only the rate"_test = [] {
		const auto tag = makeTag({{"sample_rate", 2.4e6f}, {"frequency", 915.0e6}});
		const auto meta = metaFromTag(tag, true);

		expect(!meta.sampleRate.has_value());
		expect(meta.centerHz.has_value()) << "the escape hatch is about the rate, not the frequency";
	};

	"falls back to trigger_meta_info for SigMF interop"_test = [] {
		//What gr::incubator::sigmf::SigMFSource actually produces: no frequency tag, the whole
		//SigMF metadata blob nested under trigger_meta_info
		gr::property_map nested;
		nested[gr::pmt::Value::Map::key_type("core:frequency", nested.get_allocator().resource())] = 433.0e6;

		const auto tag = makeTag({{"trigger_meta_info", nested}});
		const auto meta = metaFromTag(tag, false);

		expect(meta.centerHz.has_value());
		expect(eq(*meta.centerHz, 433.0e6));
	};

	"an explicit frequency tag beats the nested SigMF metadata"_test = [] {
		gr::property_map nested;
		nested[gr::pmt::Value::Map::key_type("core:frequency", nested.get_allocator().resource())] = 433.0e6;

		const auto tag = makeTag({{"frequency", 915.0e6}, {"trigger_meta_info", nested}});
		const auto meta = metaFromTag(tag, false);

		expect(eq(*meta.centerHz, 915.0e6))
			<< "a source that publishes both is taken at its word";
	};

	"an empty tag yields nothing"_test = [] {
		const auto meta = metaFromTag(makeTag({}), false);
		expect(!meta.Any());
	};

	"later tags in a span win"_test = [] {
		//A retune partway through a block shows the state at the end of it, which is what the
		//display actually goes on to render
		const auto first = makeTag({{"frequency", 100.0e6}});
		const auto second = makeTag({{"frequency", 200.0e6}});

		std::vector<std::pair<std::size_t, std::reference_wrapper<const gr::property_map>>> tags{
			{0UZ, std::cref(first)},
			{64UZ, std::cref(second)}};

		const auto meta = metaFromTags(tags, false);
		expect(eq(*meta.centerHz, 200.0e6));
	};

	"a span with no tags yields nothing"_test = [] {
		const std::vector<std::pair<std::size_t, std::reference_wrapper<const gr::property_map>>> tags;
		expect(!metaFromTags(tags, false).Any());
	};

	"reads the upstream loss and status tags"_test = [] {
		const auto tag = makeTag({
			{"n_dropped_samples", gr::Size_t{4096}},
			{"rx_overflow", true},
			{"end_of_stream", true},
		});
		const auto meta = metaFromTag(tag, false);

		expect(meta.upstreamDropped.has_value());
		expect(eq(*meta.upstreamDropped, std::uint64_t{4096}));
		expect(meta.overflow);
		expect(meta.endOfStream);
	};

	"a zero drop count is not a report of loss"_test = [] {
		//Every tag from some sources carries n_dropped_samples=0; treating that as a report
		//would light the warning permanently and make it worthless
		expect(!metaFromTag(makeTag({{"n_dropped_samples", gr::Size_t{0}}}), false)
			.upstreamDropped.has_value());
	};

	"a false flag is not a report"_test = [] {
		const auto meta = metaFromTag(makeTag({{"rx_overflow", false}, {"end_of_stream", false}}), false);
		expect(!meta.overflow);
		expect(!meta.endOfStream);
	};

	"event tags accumulate across a span while state tags do not"_test = [] {
		//Two overflows in one block are two overflows. Two centre frequencies are one retune,
		//and the display shows where the stream ended up.
		const auto first = makeTag({{"n_dropped_samples", gr::Size_t{100}}, {"frequency", 100.0e6}});
		const auto second = makeTag({{"n_dropped_samples", gr::Size_t{50}}, {"frequency", 200.0e6}});

		std::vector<std::pair<std::size_t, std::reference_wrapper<const gr::property_map>>> tags{
			{0UZ, std::cref(first)},
			{32UZ, std::cref(second)}};

		const auto meta = metaFromTags(tags, false);
		expect(eq(*meta.upstreamDropped, std::uint64_t{150}));
		expect(eq(*meta.centerHz, 200.0e6));
	};
};

int main() {}
