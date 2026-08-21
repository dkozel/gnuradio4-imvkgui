/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Checks the memory layout assumption the whole packed datapath rests on

	IqInjector pushes std::complex straight into a PackedIQWaveform with memcpy and no
	conversion, on the grounds that the host's layout already matches what
	PackedComplexWindow.glsl reads back. If that is ever untrue the display shows noise instead
	of a spectrum, silently. The static_asserts in IqInjector.h catch the size and endianness;
	these check the thing a static_assert cannot, which is the actual byte order of a
	constructed value.
 */

#include <bit>
#include <complex>
#include <cstdint>
#include <cstring>

#include <boost/ut.hpp>

using namespace boost::ut;

const suite<"IQ memory layout"> iqLayoutTests = [] {

	"complex<int16_t> packs I into the low half-word"_test = [] {
		const std::complex<std::int16_t> sample{1, 2};

		std::uint32_t word = 0;
		std::memcpy(&word, &sample, sizeof(word));

		//PackedComplexWindow.glsl reads I with bitfieldExtract(w, 0, 16) and Q with
		//bitfieldExtract(w, 16, 16), so I must be in the low half
		expect(eq(word, std::uint32_t{0x00020001}))
			<< "complex<int16_t>{1,2} must pack as 0x00020001";
	};

	"complex<int16_t> is four bytes with no padding"_test = [] {
		expect(eq(sizeof(std::complex<std::int16_t>), std::size_t{4}));

		//An array has to be contiguous too: the copy is one memcpy over the whole span, not
		//per element
		const std::complex<std::int16_t> samples[3] = {{1, 2}, {3, 4}, {5, 6}};
		std::uint32_t words[3] = {};
		std::memcpy(words, samples, sizeof(words));

		expect(eq(words[0], std::uint32_t{0x00020001}));
		expect(eq(words[1], std::uint32_t{0x00040003}));
		expect(eq(words[2], std::uint32_t{0x00060005}));
	};

	"negative components sign-extend into the right half-words"_test = [] {
		//The failure this catches: treating the word as two uint16 and losing the sign, which
		//would put a negative-frequency tone in the wrong bin rather than failing outright
		const std::complex<std::int16_t> sample{-1, -2};

		std::uint32_t word = 0;
		std::memcpy(&word, &sample, sizeof(word));

		expect(eq(word, std::uint32_t{0xfffeffff}));
	};

	"complex<float> is two adjacent floats"_test = [] {
		expect(eq(sizeof(std::complex<float>), std::size_t{8}));

		const std::complex<float> samples[2] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
		float floats[4] = {};
		std::memcpy(floats, samples, sizeof(floats));

		//PACKED_IQ_FLOAT32 reads din[i*2+0] as I and din[i*2+1] as Q
		expect(eq(floats[0], 1.0f));
		expect(eq(floats[1], 2.0f));
		expect(eq(floats[2], 3.0f));
		expect(eq(floats[3], 4.0f));
	};

	"host is little-endian"_test = [] {
		//The shader loads native-order words. A big-endian host would need a byte swap that
		//nothing in the datapath performs.
		expect(std::endian::native == std::endian::little);
	};
};

int main() {}
