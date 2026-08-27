/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Checks conversion of SigMF annotation tags into the display's Annotation model

	Pure header logic, no GPU and no flowgraph. Two things here are worth pinning down.

	The first is that this makes the same decisions about malformed input as BuildAnnotationSet()
	in SigMFSource.cpp. The two are separate implementations reading separate representations of
	the same SigMF fields, and if they drift then the same recording drawn through a flowgraph
	and drawn through sigmf-spectrum disagree about what it contains - silently, because both
	produce a plausible looking picture.

	The second is the coordinate system. Annotations are placed against SpectrumEngine's stream
	position, not the recording position the tag carries, and getting that backwards puts every
	box in the wrong place on any source with an offset or a loop.
 */

#include <boost/ut.hpp>

#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/imcufosphor/detail/AnnotationsFromTags.hpp>

using namespace boost::ut;
using namespace gr::imcufosphor::detail;

namespace {

///@brief Builds a property_map, working around the pmr key type
gr::property_map makeMap(std::initializer_list<std::pair<std::string_view, gr::pmt::Value>> entries)
{
	gr::property_map map;
	for(const auto& [k, v] : entries)
		map[gr::pmt::Value::Map::key_type(std::string(k), map.get_allocator().resource())] = v;
	return map;
}

/**
	@brief An annotation tag in the shape the incubator's SigMF source publishes

	Trigger name at the top level, every SigMF field nested under trigger_meta_info. Built by
	hand rather than by running a source so that this test stays a unit test of the conversion.
 */
gr::property_map makeAnnotationTag(
	std::initializer_list<std::pair<std::string_view, gr::pmt::Value>> meta)
{
	return makeMap({
		{"trigger_name", std::string(g_sigmfAnnotationTrigger)},
		{"trigger_meta_info", makeMap(meta)}});
}

} // namespace

const suite<"annotation tag conversion"> annotationTests = [] {

	"recognises the SigMF annotation trigger and nothing else"_test = [] {
		expect(isAnnotationTag(makeAnnotationTag({{"core:label", std::string("burst")}})));

		//The other two tags the same source publishes, which carry core: keys of their own and
		//would draw boxes over the whole display if they were mistaken for annotations
		expect(!isAnnotationTag(makeMap({{"trigger_name", std::string("SigMFSource::start")}})));
		expect(!isAnnotationTag(makeMap({{"trigger_name", std::string("SigMFSource::capture")}})));

		//A tag with no trigger name at all - an ordinary sample_rate tag, say
		expect(!isAnnotationTag(makeMap({{"sample_rate", 1.0e6f}})));
	};

	/**
		@brief The second producer of the same tag shape

		gr-omnisig's classifier publishes annotations in exactly this format but names its trigger
		after itself, so that a consumer can tell a recording's stored metadata from a live
		classification of the samples in front of it. The display has to accept both, and the
		conversion below has to treat them identically - the tag is the same tag.
	 */
	"recognises the OmniSIG classifier's trigger too"_test = [] {
		expect(isAnnotationTag(makeMap({
			{"trigger_name", std::string(g_omnisigAnnotationTrigger)},
			{"trigger_meta_info", makeMap({{"core:label", std::string("LTE")}})}})));

		//Still an explicit list, not a suffix test: a future block naming a trigger
		//"Something::annotation" for something that is not a SigMF annotation must not be drawn.
		expect(!isAnnotationTag(makeMap({{"trigger_name", std::string("Whatever::annotation")}})));
	};

	"converts an OmniSIG annotation the same way as a SigMF one"_test = [] {
		const auto tag = makeMap({
			{"trigger_name", std::string(g_omnisigAnnotationTrigger)},
			{"trigger_meta_info", makeMap({
				{"core:sample_start", std::uint64_t{999999}},
				{"core:sample_count", std::uint64_t{802816}},
				{"core:freq_lower_edge", 2466907137.0},
				{"core:freq_upper_edge", 2467879140.0},
				{"core:label", std::string("LoRa")},
				{"deepsig:confidence", 0.9156636595726013},
			})}});

		const auto ann = annotationFromTag(tag, 4096);
		expect(ann.has_value()) >> fatal;

		//Placed at the stream position, exactly as a SigMF source's annotation is. The classifier's
		//core:sample_start is a position in the recording it was told about, not in this stream.
		expect(eq(ann->sampleStart, std::int64_t{4096}));
		expect(eq(ann->sampleEnd, std::int64_t{4096 + 802816}));
		expect(ann->hasFreq);
		expect(eq(ann->label, std::string("LoRa")));
	};

	"converts a fully populated annotation"_test = [] {
		const auto tag = makeAnnotationTag({
			{"core:sample_start", std::uint64_t{1000}},
			{"core:sample_count", std::uint64_t{500}},
			{"core:freq_lower_edge", 914.0e6},
			{"core:freq_upper_edge", 916.0e6},
			{"core:label", std::string("burst")},
			{"core:description", std::string("a description")},
			{"core:comment", std::string("a comment")},
			{"core:generator", std::string("a generator")},
		});

		const auto ann = annotationFromTag(tag, 4096);
		expect(ann.has_value()) >> fatal;

		//Placed at the stream position, not at core:sample_start. The tag says the annotation
		//begins at recording sample 1000; the display draws it where those samples actually
		//entered the pipeline.
		expect(eq(ann->sampleStart, std::int64_t{4096}));
		expect(eq(ann->sampleEnd, std::int64_t{4096 + 500}))
			<< "duration is the one thing that has to come from the tag";

		expect(ann->hasFreq);
		expect(eq(ann->freqLoHz, 914.0e6));
		expect(eq(ann->freqHiHz, 916.0e6));

		expect(ann->label == "burst");
		expect(ann->description == "a description");
		expect(ann->comment == "a comment");
		expect(ann->generator == "a generator");

		expect(eq(ann->colorKey, ColorKeyForLabel("burst")));
	};

	"an absent sample_count is a point, not a span"_test = [] {
		//core:sample_count is optional in the schema. BuildAnnotationSet() represents its
		//absence as a zero length annotation and the overlay draws that as a line; if this
		//diverged, the same recording would render differently through the two paths.
		const auto ann = annotationFromTag(
			makeAnnotationTag({{"core:label", std::string("tick")}}), 100);

		expect(ann.has_value()) >> fatal;
		expect(eq(ann->sampleStart, std::int64_t{100}));
		expect(eq(ann->sampleEnd, std::int64_t{100}));

		//...and the set widens it to one sample for overlap purposes, in one place only
		expect(eq(AnnotationSet::EffectiveEnd(*ann), std::int64_t{101}));
	};

	"a nonsensical sample_count degrades to a point"_test = [] {
		for(const auto& bad : {gr::pmt::Value(std::int64_t{-5}), gr::pmt::Value(0.0)})
		{
			auto meta = makeMap({{"core:label", std::string("x")}});
			meta[gr::pmt::Value::Map::key_type("core:sample_count", meta.get_allocator().resource())] = bad;

			const auto tag = makeMap({
				{"trigger_name", std::string(g_sigmfAnnotationTrigger)},
				{"trigger_meta_info", meta}});

			const auto ann = annotationFromTag(tag, 100);
			expect(ann.has_value()) >> fatal;
			expect(eq(ann->sampleEnd, std::int64_t{100}))
				<< "a negative or zero span is a data error, not a negative height box";
		}
	};

	"reversed frequency edges are swapped rather than drawn backwards"_test = [] {
		const auto ann = annotationFromTag(makeAnnotationTag({
			{"core:freq_lower_edge", 916.0e6},
			{"core:freq_upper_edge", 914.0e6}}), 0);

		expect(ann.has_value()) >> fatal;
		expect(ann->hasFreq);
		expect(eq(ann->freqLoHz, 914.0e6));
		expect(eq(ann->freqHiHz, 916.0e6));
	};

	"one frequency edge is no band at all"_test = [] {
		//Half a band is not a band. An annotation without a frequency extent still says
		//something about a time range and is drawn full width, which is the useful reading.
		const auto lower = annotationFromTag(
			makeAnnotationTag({{"core:freq_lower_edge", 914.0e6}}), 0);
		expect(lower.has_value()) >> fatal;
		expect(!lower->hasFreq);

		const auto upper = annotationFromTag(
			makeAnnotationTag({{"core:freq_upper_edge", 916.0e6}}), 0);
		expect(upper.has_value()) >> fatal;
		expect(!upper->hasFreq);
	};

	"an unlabelled annotation gets the reserved colour key"_test = [] {
		const auto ann = annotationFromTag(makeAnnotationTag({
			{"core:comment", std::string("no label here")}}), 0);

		expect(ann.has_value()) >> fatal;
		expect(ann->label.empty());
		expect(eq(ann->colorKey, g_unlabelledColorKey))
			<< "so the overlay draws it neutral grey rather than picking a hue from an empty string";
	};

	"the same label keeps the same colour"_test = [] {
		//Both panes resolve the key to a colour independently, so two annotations from the same
		//emitter must agree on the number rather than on what red is
		const auto a = annotationFromTag(makeAnnotationTag({{"core:label", std::string("dect")}}), 0);
		const auto b = annotationFromTag(makeAnnotationTag({{"core:label", std::string("dect")}}), 9999);
		const auto c = annotationFromTag(makeAnnotationTag({{"core:label", std::string("lora")}}), 0);

		expect(a.has_value() && b.has_value() && c.has_value()) >> fatal;
		expect(eq(a->colorKey, b->colorKey));
		expect(neq(a->colorKey, c->colorKey));
	};

	"a tag with no metadata blob yields nothing"_test = [] {
		//The trigger name says annotation but there is nothing to place. Skipping beats
		//inventing a zero width box at the current position.
		const auto tag = makeMap({{"trigger_name", std::string(g_sigmfAnnotationTrigger)}});
		expect(!annotationFromTag(tag, 0).has_value());
	};

	"a negative stream position yields nothing"_test = [] {
		//The port reports a tag's index relative to the current stream position and one
		//published before it reads below zero. Such a tag describes samples this block has
		//already passed, so there is nothing to place it against.
		expect(!annotationFromTag(makeAnnotationTag({{"core:label", std::string("x")}}), -1).has_value());
	};

	"converted annotations are queryable as a set"_test = [] {
		//The conversion exists to feed AnnotationSet, so check the two work together: overlap
		//queries are what the overlay actually calls, and they are only valid after Finalize().
		AnnotationSet set;

		const auto add = [&set](std::int64_t pos, std::uint64_t count, const char* label) {
			const auto ann = annotationFromTag(makeAnnotationTag({
				{"core:sample_count", count},
				{"core:label", std::string(label)}}), pos);
			expect(ann.has_value()) >> fatal;
			set.Add(*ann);
		};

		//Deliberately added out of order: the source sorts its schedule by offset, but nothing
		//in this path depends on that and Finalize() is what establishes the invariant.
		add(5000, 100, "third");
		add(0, 1000, "first");
		add(2000, 500, "second");

		set.Finalize();
		expect(eq(set.size(), 3UZ));

		std::vector<std::size_t> hits;
		set.QueryOverlapping(1900, 2100, hits);
		expect(eq(hits.size(), 1UZ)) >> fatal;
		expect(set[hits[0]].label == "second");

		//A window covering everything finds all three
		hits.clear();
		set.QueryOverlapping(0, 10000, hits);
		expect(eq(hits.size(), 3UZ));

		//A window in the gap between them finds none
		hits.clear();
		set.QueryOverlapping(3000, 4000, hits);
		expect(eq(hits.size(), 0UZ));
	};

	"a looping source places each pass separately"_test = [] {
		//A repeating source republishes its whole schedule on every wrap, and because
		//annotations are placed against the stream rather than the recording, the second pass
		//lands further down the waterfall instead of on top of the first. AnnotationOverlay
		//documents expecting to see one annotation several times over for this reason.
		const auto first = annotationFromTag(
			makeAnnotationTag({{"core:sample_count", std::uint64_t{100}},
				{"core:label", std::string("burst")}}), 1000);
		const auto second = annotationFromTag(
			makeAnnotationTag({{"core:sample_count", std::uint64_t{100}},
				{"core:label", std::string("burst")}}), 51000);

		expect(first.has_value() && second.has_value()) >> fatal;
		expect(neq(first->sampleStart, second->sampleStart));
		expect(eq(first->colorKey, second->colorKey)) << "same emitter, same colour on both passes";

		AnnotationSet set;
		set.Add(*first);
		set.Add(*second);
		set.Finalize();

		std::vector<std::size_t> hits;
		set.QueryOverlapping(50900, 51200, hits);
		expect(eq(hits.size(), 1UZ)) << "the second pass is found on its own, not merged with the first";
	};
};

int main() {}
