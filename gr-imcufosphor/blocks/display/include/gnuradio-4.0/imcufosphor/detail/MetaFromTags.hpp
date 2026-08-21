/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Extraction of sample rate and centre frequency from stream tags
 */
#ifndef GR_IMCUFOSPHOR_META_FROM_TAGS_HPP
#define GR_IMCUFOSPHOR_META_FROM_TAGS_HPP

#include <optional>
#include <string>
#include <string_view>

#include <gnuradio-4.0/Tag.hpp>

namespace gr::imcufosphor::detail {

/**
	@brief Reads a numeric tag value whatever arithmetic type it was written as

	Tag values are a pmt variant and the writer picks the type. gr::tag::SAMPLE_RATE is declared
	float and gr::tag::FREQUENCY double, but a block filling a property_map by hand can easily
	produce uint64_t or int64_t for either, and a display that silently ignored those would show
	a wrong axis rather than fail. So every arithmetic alternative is accepted and widened.

	@return The value as a double, or nothing if the key is absent or not numeric
 */
[[nodiscard]] inline std::optional<double> tagValueAsDouble(
	const gr::property_map& map,
	std::string_view key)
{
	//The map is pmr-allocated, so a key has to be constructed against its resource rather than
	//looked up with a bare std::string
	const auto it = map.find(
		gr::pmt::Value::Map::key_type(std::string(key), map.get_allocator().resource()));
	if(it == map.end())
		return std::nullopt;

	const auto& v = it->second;

	//Every width, not just the 64-bit ones. gr::Size_t is uint32_t, so a tag as ordinary as
	//n_dropped_samples arrives as a 32-bit value and a coercion that only tried uint64_t
	//silently reported "no such tag" - which is how this list came to be exhaustive.
	std::optional<double> out;
	const auto tryAs = [&]<typename TAlt>() {
		if(!out.has_value())
		{
			if(auto p = v.template get_if<TAlt>())
				out = static_cast<double>(*p);
		}
	};

	tryAs.template operator()<double>();
	tryAs.template operator()<float>();
	tryAs.template operator()<std::uint64_t>();
	tryAs.template operator()<std::int64_t>();
	tryAs.template operator()<std::uint32_t>();
	tryAs.template operator()<std::int32_t>();
	tryAs.template operator()<std::uint16_t>();
	tryAs.template operator()<std::int16_t>();
	tryAs.template operator()<std::uint8_t>();
	tryAs.template operator()<std::int8_t>();

	return out;
}

/**
	@brief Reads a boolean tag value

	Separate from the numeric coercion because a bool is not usefully a number here: a flag tag
	written as the integer 1 means the same as true, but a flag written as 0.5 means nothing, so
	only bool and the integral types are accepted.
 */
[[nodiscard]] inline std::optional<bool> boolFromTag(
	const gr::property_map& map,
	std::string_view key)
{
	const auto it = map.find(
		gr::pmt::Value::Map::key_type(std::string(key), map.get_allocator().resource()));
	if(it == map.end())
		return std::nullopt;

	const auto& v = it->second;
	if(auto b = v.template get_if<bool>())
		return *b;

	//A flag written as an integer, in whatever width the writer picked. Falls through to the
	//numeric coercion so this stays in step with it rather than drifting.
	if(auto n = tagValueAsDouble(map, key))
		return (*n != 0.0);

	return std::nullopt;
}

///@brief What a display needs to know about the stream, when the stream bothers to say
struct TagMeta
{
	std::optional<double> sampleRate;
	std::optional<double> centerHz;

	/**
		@brief Samples the upstream chain admits to having lost

		Distinct from the display's own drop counter: those are samples this block chose not to
		show because the render thread was behind, which is normal and expected. These went
		missing before the block ever saw them, which means the spectrum has a gap in it.
	 */
	std::optional<std::uint64_t> upstreamDropped;

	///@brief Set when the stream reports a receiver overflow
	bool overflow = false;

	/**
		@brief Set when the stream has ended

		A display should freeze on this rather than clear: the last thing shown is the last
		thing that was there, and blanking it destroys the only remaining view of it.
	 */
	bool endOfStream = false;

	[[nodiscard]] bool Any() const
	{
		return sampleRate.has_value() || centerHz.has_value() ||
			upstreamDropped.has_value() || overflow || endOfStream;
	}
};

/**
	@brief Pulls the sample rate and centre frequency out of one tag map

	@par Centre frequency precedence

	-# @c "frequency" - gr::tag::FREQUENCY, the upstream default tag (Tag.hpp:205), declared
	   double and Hz. This is the convention; nothing here invents one.
	-# @c trigger_meta_info["core:frequency"] - SigMF interop. The incubator's SigMF source
	   folds every SigMF global and capture key into a nested map under this tag rather than
	   publishing a frequency tag, so a graph reading a recording only ever produces this form.

	Sample rate comes from @c "sample_rate" (gr::tag::SAMPLE_RATE) and can be ignored entirely,
	which is the escape hatch for a graph whose upstream rate tag is wrong or refers to a
	different point in the chain.
 */
[[nodiscard]] inline TagMeta metaFromTag(const gr::property_map& tag, bool ignoreSampleRate)
{
	TagMeta meta;

	if(!ignoreSampleRate)
	{
		if(auto sr = tagValueAsDouble(tag, gr::tag::SAMPLE_RATE.shortKey()))
		{
			//A zero or negative rate would make every derived quantity - the frequency axis,
			//the density map's decay constants, the time axis - either wrong or a division by
			//zero. Treat it as absent.
			if(*sr > 0)
				meta.sampleRate = *sr;
		}
	}

	if(auto cf = tagValueAsDouble(tag, gr::tag::FREQUENCY.shortKey()))
		meta.centerHz = *cf;

	if(auto nd = tagValueAsDouble(tag, gr::tag::N_DROPPED_SAMPLES.shortKey()); nd && (*nd > 0))
		meta.upstreamDropped = static_cast<std::uint64_t>(*nd);

	if(auto ov = boolFromTag(tag, gr::tag::RX_OVERFLOW.shortKey()); ov.value_or(false))
		meta.overflow = true;

	if(auto eos = boolFromTag(tag, gr::tag::END_OF_STREAM.shortKey()); eos.value_or(false))
		meta.endOfStream = true;

	//Only consulted when there is no frequency tag, so a source that publishes both is taken
	//at its word rather than being overridden by its own metadata blob
	if(!meta.centerHz.has_value())
	{
		const auto it = tag.find(gr::pmt::Value::Map::key_type(
			std::string(gr::tag::TRIGGER_META_INFO.shortKey()), tag.get_allocator().resource()));
		if(it != tag.end())
		{
			if(auto nested = it->second.template get_if<gr::property_map>())
			{
				if(auto cf = tagValueAsDouble(*nested, "core:frequency"))
					meta.centerHz = *cf;
			}
		}
	}

	return meta;
}

/**
	@brief Merges the metadata from every tag in a span, later tags winning

	A block arrives with any number of tags at any offsets. The display has one frequency axis
	and one sample rate, so it shows the state at the end of the block; a retune partway through
	is visible as a discontinuity in the waterfall, which is what it actually is.
 */
template<typename TagRange>
[[nodiscard]] TagMeta metaFromTags(TagRange&& tags, bool ignoreSampleRate)
{
	TagMeta meta;

	for(const auto& entry : tags)
	{
		const TagMeta m = metaFromTag(std::get<1>(entry).get(), ignoreSampleRate);

		//Last one wins for the two that describe the current state of the stream
		if(m.sampleRate.has_value())
			meta.sampleRate = m.sampleRate;
		if(m.centerHz.has_value())
			meta.centerHz = m.centerHz;

		//...but the three that report events accumulate, because a block carrying two overflow
		//tags had two overflows and reporting only the last would undercount
		if(m.upstreamDropped.has_value())
			meta.upstreamDropped = meta.upstreamDropped.value_or(0) + *m.upstreamDropped;
		meta.overflow = meta.overflow || m.overflow;
		meta.endOfStream = meta.endOfStream || m.endOfStream;
	}

	return meta;
}

} // namespace gr::imcufosphor::detail

#endif
