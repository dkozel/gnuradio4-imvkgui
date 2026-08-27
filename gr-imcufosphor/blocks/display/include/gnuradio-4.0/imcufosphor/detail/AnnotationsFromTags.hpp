/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Conversion of SigMF annotation tags into the display's Annotation model
 */
#ifndef GR_IMCUFOSPHOR_ANNOTATIONS_FROM_TAGS_HPP
#define GR_IMCUFOSPHOR_ANNOTATIONS_FROM_TAGS_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gnuradio-4.0/Tag.hpp>

#include "Annotation.h"

#include <gnuradio-4.0/imcufosphor/detail/MetaFromTags.hpp>

namespace gr::imcufosphor::detail {

/**
	@brief Trigger name the incubator's SigMF source publishes on an annotation

	Matched exactly rather than by suffix. A display that guessed - treating any tag whose
	metadata happens to carry a @c core: key as an annotation - would draw boxes for capture
	tags and for whatever a future source puts in a trigger, and the failure mode is silent
	clutter over the waveform rather than an error anyone would investigate.
 */
inline constexpr std::string_view g_sigmfAnnotationTrigger = "SigMFSource::annotation";

/**
	@brief Trigger name gr-omnisig's classifier publishes

	A second producer of the same tag shape. gr-omnisig names its trigger after itself rather than
	borrowing the SigMF source's name, and it is right to: a consumer that wants to know whether an
	annotation came from a recording's stored metadata or from a live classification of the samples in
	front of it has nowhere else to look.

	Which is why this is an explicit list and not a prefix test. Exact matching is what keeps capture
	tags out of the overlay, and that property is worth more than the convenience of matching anything
	ending in "::annotation" - a name a future block could pick for something that is not a SigMF
	annotation at all.
 */
inline constexpr std::string_view g_omnisigAnnotationTrigger = "OmniSIGClassifier::annotation";

///@brief Every trigger name that carries a SigMF annotation in trigger_meta_info
inline constexpr std::array<std::string_view, 2> g_annotationTriggers{
	g_sigmfAnnotationTrigger,
	g_omnisigAnnotationTrigger};

/**
	@brief Reads a string tag value

	Kept beside tagValueAsDouble() for symmetry, but the type question is settled rather than
	open: pmt::Value stores every string as std::pmr::string on its own memory resource, and
	get_if<std::string> is explicitly constrained out of the interface (Value.hpp:446). So there
	is exactly one alternative to try, and the copy out to std::string is unavoidable.
 */
[[nodiscard]] inline std::optional<std::string> stringFromTag(
	const gr::property_map& map,
	std::string_view key)
{
	const auto it = map.find(
		gr::pmt::Value::Map::key_type(std::string(key), map.get_allocator().resource()));
	if(it == map.end())
		return std::nullopt;

	if(auto s = it->second.template get_if<std::pmr::string>())
		return std::string(s->begin(), s->end());

	return std::nullopt;
}

///@brief True if this tag is a SigMF annotation, from any of the blocks that produce them
[[nodiscard]] inline bool isAnnotationTag(const gr::property_map& tag)
{
	const auto name = stringFromTag(tag, gr::tag::TRIGGER_NAME.shortKey());
	if(!name.has_value())
		return false;

	return std::ranges::find(g_annotationTriggers, *name) != g_annotationTriggers.end();
}

/**
	@brief Converts one annotation tag into the display-neutral model

	The GR4 counterpart of BuildAnnotationSet() in SigMFSource.cpp, and deliberately makes the
	same decisions about malformed input, so that a recording drawn through a flowgraph and the
	same recording drawn through sigmf-spectrum do not disagree about what it contains.

	@par Why @a streamPos and not core:sample_start

	The model's sample coordinates have to match whatever the display is placing them against,
	and for a flowgraph that is SpectrumEngine's stream position - a monotonic count of samples
	actually pushed into the pipeline (SpectrumEngine.cpp:269 reports it in both slots of
	BlockSpan, because a live source has no file position). The tag's own core:sample_start is
	a position in the *recording*, which is a different number as soon as the source is given a
	non-zero offset, and a different number again on every loop of a repeating source.

	Placing against the stream is also what makes looped playback right rather than merely
	tolerable: the source republishes its whole tag schedule on each wrap, so the annotation
	arrives again at a new stream position and is drawn again over the rows it actually
	describes. AnnotationOverlay already expects to see one annotation several times over for
	exactly this reason - see GetLastMatchedCount().

	The duration, though, has nowhere to come from but the tag, which is why core:sample_count
	has to survive the trip. It is the same number in either coordinate system.

	@param tag			The tag map, already known to be an annotation
	@param streamPos	Stream index of the sample this tag sits on

	@return The annotation, or nothing if the tag carries nothing placeable
 */
[[nodiscard]] inline std::optional<::Annotation> annotationFromTag(
	const gr::property_map& tag,
	std::int64_t streamPos)
{
	if(streamPos < 0)
		return std::nullopt;

	//Everything a SigMF source knows about an annotation beyond its position is folded into the
	//nested metadata blob, the same place metaFromTag() finds core:frequency.
	const auto it = tag.find(gr::pmt::Value::Map::key_type(
		std::string(gr::tag::TRIGGER_META_INFO.shortKey()), tag.get_allocator().resource()));
	if(it == tag.end())
		return std::nullopt;

	const auto* meta = it->second.template get_if<gr::property_map>();
	if(meta == nullptr)
		return std::nullopt;

	::Annotation ann;
	ann.sampleStart = streamPos;

	//core:sample_count is optional in the schema. Absent means a point in time rather than a
	//span, which the model represents as a zero length annotation and the overlay draws as a
	//line - the same treatment BuildAnnotationSet() gives it.
	if(const auto count = tagValueAsDouble(*meta, "core:sample_count"))
	{
		//Guard the conversion rather than trusting the producer. A negative count is a data
		//error and a non-finite one would make sampleEnd meaningless; both become a point.
		if(std::isfinite(*count) && (*count > 0))
			ann.sampleEnd = streamPos + static_cast<std::int64_t>(*count);
		else
			ann.sampleEnd = streamPos;
	}
	else
		ann.sampleEnd = streamPos;

	//The two frequency edges are independently optional. Both present is the only case that
	//describes a band; one alone is not half a band, it is no band.
	const auto lo = tagValueAsDouble(*meta, "core:freq_lower_edge");
	const auto hi = tagValueAsDouble(*meta, "core:freq_upper_edge");
	if(lo.has_value() && hi.has_value() && std::isfinite(*lo) && std::isfinite(*hi))
	{
		//Reversed edges are a data error, not a negative width rectangle
		ann.freqLoHz = std::min(*lo, *hi);
		ann.freqHiHz = std::max(*lo, *hi);
		ann.hasFreq = true;
	}

	ann.label = stringFromTag(*meta, "core:label").value_or(std::string());
	ann.description = stringFromTag(*meta, "core:description").value_or(std::string());
	ann.comment = stringFromTag(*meta, "core:comment").value_or(std::string());
	ann.generator = stringFromTag(*meta, "core:generator").value_or(std::string());
	ann.colorKey = ColorKeyForLabel(ann.label);

	return ann;
}

} // namespace gr::imcufosphor::detail

#endif
