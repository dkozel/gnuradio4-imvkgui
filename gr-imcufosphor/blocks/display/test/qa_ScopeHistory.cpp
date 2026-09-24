/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Checks the scope's pre-trigger sample ring

	No GPU: ScopeHistory is plain floats and a stream coordinate, which is the whole reason the
	capture path was split around it. See qa_ScopeTrigger.cpp for why that line is where it is.

	The two properties worth pinning are the ones a caller cannot see when they go wrong: a gather
	that straddles the wrap point must return the same values as one that does not, and a gather
	of a range that has aged out must fail rather than return some of it.
 */

#include <boost/ut.hpp>

#include <complex>
#include <cstdint>
#include <vector>

#include "ScopeHistory.h"

using namespace boost::ut;

namespace {

std::vector<float> Ramp(std::size_t n, float from = 0)
{
	std::vector<float> v(n);
	for(std::size_t i = 0; i < n; i++)
		v[i] = from + static_cast<float>(i);
	return v;
}

} // namespace

const suite<"ScopeHistory"> historyTests = [] {

	"an empty history is resident in nothing"_test = [] {
		ScopeHistory h;
		h.Resize(16, 1);

		expect(eq(h.Begin(), std::uint64_t{0}));
		expect(eq(h.End(), std::uint64_t{0}));
		expect(!h.Resident(0, 1));
	};

	"a short append is readable back exactly"_test = [] {
		ScopeHistory h;
		h.Resize(64, 1);

		const auto in = Ramp(10);
		h.Append(in);

		expect(eq(h.End(), std::uint64_t{10}));
		expect(eq(h.Begin(), std::uint64_t{0}));

		std::vector<float> out(10);
		expect(h.Gather(0, 10, 0, out));
		expect(std::ranges::equal(in, out));
	};

	"Gather across the wrap point returns the same values as before it"_test = [] {
		//3.5 depths of ramp, so the resident window is guaranteed to straddle the seam
		ScopeHistory h;
		h.Resize(64, 1);

		const auto in = Ramp(224);
		for(std::size_t i = 0; i < in.size(); i += 7)
			h.Append(std::span(in).subspan(i, std::min<std::size_t>(7, in.size() - i)));

		expect(eq(h.End(), std::uint64_t{224}));
		expect(eq(h.Begin(), std::uint64_t{224 - 64}));

		std::vector<float> out(64);
		expect(h.Gather(h.Begin(), 64, 0, out));

		for(std::size_t i = 0; i < 64; i++)
			expect(eq(out[i], static_cast<float>(160 + i))) << "at offset " << i;
	};

	"Gather refuses a range that has aged out and leaves the output untouched"_test = [] {
		//A partial record is not a degraded picture of the signal, it is a picture of a different
		//signal. Refusing is the only honest answer.
		ScopeHistory h;
		h.Resize(32, 1);
		h.Append(Ramp(100));

		std::vector<float> out(16, -12345.0f);
		expect(!h.Gather(0, 16, 0, out)) << "sample 0 fell out of the ring long ago";
		for(float v : out)
			expect(eq(v, -12345.0f)) << "and nothing was written";

		expect(!h.Gather(90, 16, 0, out)) << "and 90..106 runs past the newest sample";
	};

	"an append larger than the depth keeps the newest samples"_test = [] {
		ScopeHistory h;
		h.Resize(16, 1);
		h.Append(Ramp(100));

		expect(eq(h.End(), std::uint64_t{100}));
		expect(eq(h.Begin(), std::uint64_t{84}));

		std::vector<float> out(16);
		expect(h.Gather(84, 16, 0, out));
		for(std::size_t i = 0; i < 16; i++)
			expect(eq(out[i], static_cast<float>(84 + i)));
	};

	"the stream coordinate is monotonic across Clear and Resize"_test = [] {
		//notes/time-domain-plan.md section 1: a display's time base must never go backwards, or
		//everything recorded against it before the reset compares wrongly afterwards.
		ScopeHistory h;
		h.Resize(64, 1);
		h.Append(Ramp(200));
		expect(eq(h.End(), std::uint64_t{200}));

		h.Clear();
		expect(eq(h.End(), std::uint64_t{200})) << "Clear drops the samples, not the clock";
		expect(eq(h.Begin(), std::uint64_t{200})) << "and Begin catches up to it";
		expect(eq(h.GetCount(), std::uint64_t{0}));

		h.Resize(128, 1);
		expect(eq(h.End(), std::uint64_t{200})) << "a resize is not a rewind either";

		h.Append(Ramp(10));
		expect(eq(h.End(), std::uint64_t{210}));
		expect(eq(h.Begin(), std::uint64_t{200}));
	};

	"the complex stride reads I and Q out of one buffer"_test = [] {
		//I and Q of one port share a history precisely so they cannot desynchronise. Only
		//inter-port alignment is at risk, and joint admission in the sink handles that.
		ScopeHistory h;
		h.Resize(64, 2);

		std::vector<std::complex<float>> in(40);
		for(std::size_t i = 0; i < in.size(); i++)
			in[i] = std::complex<float>(static_cast<float>(i), -static_cast<float>(i));

		h.Append(std::span<const float>(reinterpret_cast<const float*>(in.data()), in.size() * 2));
		expect(eq(h.End(), std::uint64_t{40})) << "counted in samples, not floats";

		std::vector<float> re(40);
		std::vector<float> im(40);
		expect(h.Gather(0, 40, 0, re));
		expect(h.Gather(0, 40, 1, im));

		for(std::size_t i = 0; i < 40; i++)
		{
			expect(eq(re[i], static_cast<float>(i)));
			expect(eq(im[i], -static_cast<float>(i)));
		}
	};

	"GatherMagSquared is the squared envelope"_test = [] {
		ScopeHistory h;
		h.Resize(64, 2);

		std::vector<std::complex<float>> in{{3, 4}, {0, 0}, {1, 0}, {0, 2}};
		h.Append(std::span<const float>(reinterpret_cast<const float*>(in.data()), in.size() * 2));

		std::vector<float> m2(4);
		expect(h.GatherMagSquared(0, 4, m2));
		expect(eq(m2[0], 25.0f));
		expect(eq(m2[1], 0.0f));
		expect(eq(m2[2], 1.0f));
		expect(eq(m2[3], 4.0f));
	};

	"GatherMagSquared refuses a real history"_test = [] {
		ScopeHistory h;
		h.Resize(64, 1);
		h.Append(Ramp(10));

		std::vector<float> m2(4);
		expect(!h.GatherMagSquared(0, 4, m2)) << "there is no Q to square";
	};

	"a component past the end is refused"_test = [] {
		ScopeHistory h;
		h.Resize(64, 1);
		h.Append(Ramp(10));

		std::vector<float> out(4);
		expect(!h.Gather(0, 4, 1, out));
	};
};

int main() {}
