/***********************************************************************************************************************
*                                                                                                                      *
* imcufosphor                                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@brief Checks that IqRing carries sample values through, not just sample counts

	Every other test of the sinks uses a constant source, which is exactly the input that cannot
	tell a working ring from one that repeats a single sample 2048 times. These push a ramp and
	check the values come back in order.

	PushUpTo() in particular is barely exercised anywhere else: the three spectrum sinks all use
	Push(), and AnalyzerSink only reaches PushUpTo() in its backpressure mode, which is off for
	everything except a file. ScopeSink uses it on every call.
 */

#include <boost/ut.hpp>

#include <complex>
#include <cstdint>
#include <vector>

#include <gnuradio-4.0/imcufosphor/detail/IqRing.hpp>

using namespace boost::ut;
using gr::imcufosphor::detail::IqRing;
using TSample = std::complex<float>;

namespace {

///@brief Sample k carries its own index, so any reordering or duplication is visible
TSample Marker(std::size_t k)
{ return TSample(static_cast<float>(k), -static_cast<float>(k)); }

} // namespace

const suite<"IqRing value fidelity"> ringTests = [] {

	"a single push round-trips in order"_test = [] {
		IqRing<TSample> ring;
		ring.Resize(4096);

		std::vector<TSample> in(1000);
		for(std::size_t i = 0; i < in.size(); i++)
			in[i] = Marker(i);

		expect(eq(ring.PushUpTo(in), std::size_t{1000}));
		expect(eq(ring.Available(), std::size_t{1000}));

		std::vector<TSample> out;
		expect(ring.WithBlock(1000, [&](std::span<const TSample> s) {
			out.assign(s.begin(), s.end());
		}));

		expect(eq(out.size(), std::size_t{1000}));
		std::size_t mismatches = 0;
		for(std::size_t i = 0; i < out.size(); i++)
		{
			if(out[i] != Marker(i))
				mismatches++;
		}
		expect(eq(mismatches, std::size_t{0}));
	};

	"many small PushUpTo calls fill the ring with distinct samples"_test = [] {
		//This is ScopeSink's actual traffic pattern. An unpaced source hands processBulk three
		//to five samples at a time, so a 2048-sample drain is assembled from ~500 pushes. If
		//PushUpTo wrote to the same place every time, or if the writer position did not advance,
		//the ring would come back holding one value repeated - which is exactly what the scope
		//displayed, and what no constant-source test could ever have caught.
		IqRing<TSample> ring;
		ring.Resize(4096);

		std::size_t pushed = 0;
		while(pushed < 2048)
		{
			const std::size_t chunk = 4;
			std::vector<TSample> in(chunk);
			for(std::size_t i = 0; i < chunk; i++)
				in[i] = Marker(pushed + i);

			const auto taken = ring.PushUpTo(in);
			expect(eq(taken, chunk)) << "the ring has room, so all four must be accepted";
			pushed += taken;
		}

		expect(eq(ring.Available(), std::size_t{2048}));

		std::vector<TSample> out;
		expect(ring.WithBlock(2048, [&](std::span<const TSample> s) {
			out.assign(s.begin(), s.end());
		}));

		std::size_t mismatches = 0;
		for(std::size_t i = 0; i < out.size(); i++)
		{
			if(out[i] != Marker(i))
				mismatches++;
		}
		expect(eq(mismatches, std::size_t{0}))
			<< "every sample distinct and in order, not one value repeated";
	};

	"drain and refill cycles stay in order"_test = [] {
		//The steady state: the reader empties the ring every frame and the writer refills it in
		//small pushes. Run enough cycles to wrap the underlying buffer several times.
		IqRing<TSample> ring;
		ring.Resize(2048);

		std::size_t produced = 0;
		std::size_t consumed = 0;
		std::size_t mismatches = 0;

		for(int cycle = 0; cycle < 20; cycle++)
		{
			while(ring.Room() > 0)
			{
				const std::size_t chunk = std::min<std::size_t>(4, ring.Room());
				std::vector<TSample> in(chunk);
				for(std::size_t i = 0; i < chunk; i++)
					in[i] = Marker(produced + i);
				produced += ring.PushUpTo(in);
			}

			const std::size_t avail = ring.Available();
			expect(gt(avail, std::size_t{0}));

			expect(ring.WithBlock(avail, [&](std::span<const TSample> s) {
				for(std::size_t i = 0; i < s.size(); i++)
				{
					if(s[i] != Marker(consumed + i))
						mismatches++;
				}
			}));
			consumed += avail;
		}

		expect(eq(mismatches, std::size_t{0}));
		expect(eq(produced, consumed)) << "everything pushed was read back exactly once";
		expect(gt(consumed, std::size_t{20000})) << "and the buffer wrapped many times";
	};

	"PushUpTo reports the shortfall when the ring is full"_test = [] {
		IqRing<TSample> ring;
		ring.Resize(1024);

		std::vector<TSample> in(4096);
		for(std::size_t i = 0; i < in.size(); i++)
			in[i] = Marker(i);

		const auto taken = ring.PushUpTo(in);
		expect(le(taken, std::size_t{4096}));
		expect(gt(taken, std::size_t{0}));
		expect(eq(ring.Dropped(), std::uint64_t{0}))
			<< "PushUpTo never drops; it reports and lets the caller decide";

		//A second push with no room takes nothing rather than overwriting
		expect(eq(ring.PushUpTo(in), std::size_t{0}));
	};
};

int main() {}
