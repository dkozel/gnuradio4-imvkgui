/*
	Diagnostic: can libsigmf parse the real-world recordings in the demo dataset?

	Answers the gating question for SigMFSource before any of it is written. The demo set
	carries unknown namespaces (deepsig:, csw:, spatial:, traceability:), a malformed
	"core::datetime" key in dect6, and metadata files with no matching data file.

	Note this deliberately does NOT use sigmf::metadata_file_to_json(). That helper is not
	a template - it hardcodes a record type of core+antenna+capture_details+signal - so
	using it would dictate our record type. Parsing into our own type is three lines.

	Build:
	  g++ -std=c++17 -o probe probe.cpp -I../../lib/libsigmf/src \
	      -I../../build/lib/libsigmf/include \
	      -I../../lib/libsigmf/external/flatbuffers/include \
	      -I../../lib/libsigmf/external/json/include -w
 */
#include "sigmf_core_generated.h"
#include "sigmf.h"
#include "sigmf_helpers.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using Record = sigmf::SigMF<
	sigmf::Global<sigmf::core::DescrT>,
	sigmf::Capture<sigmf::core::DescrT>,
	sigmf::Annotation<sigmf::core::DescrT> >;

int main(int argc, char** argv)
{
	for(int i=1; i<argc; i++)
	{
		printf("%-42s ", argv[i]);
		fflush(stdout);

		try
		{
			std::ifstream fp(argv[i]);
			if(!fp)
			{
				printf("CANNOT OPEN\n");
				continue;
			}
			std::ostringstream buf;
			buf << fp.rdbuf();

			Record rec;
			rec = nlohmann::json::parse(buf.str());

			auto& g = rec.global.get<sigmf::core::DescrT>();

			printf("OK dt=%-8s sr=%-11g caps=%-2zu anns=%-5zu ssz=%u",
				g.datatype.c_str(),
				g.sample_rate.value_or(-1),
				rec.captures.size(),
				rec.annotations.size(),
				g.datatype.empty() ? 0 : sigmf::get_sample_size(g.datatype));

			//Center frequency of the first capture, and whether every capture has one
			if(!rec.captures.empty())
			{
				auto& c = rec.captures[0].get<sigmf::core::DescrT>();
				if(c.frequency.has_value())
					printf(" f0=%.3f", c.frequency.value());
				else
					printf(" f0=ABSENT");
			}

			//How many annotations carry a usable frequency box (phase 2 needs these)
			size_t boxed = 0;
			for(auto& a : rec.annotations)
			{
				auto& av = a.get<sigmf::core::DescrT>();
				if(av.freq_lower_edge.has_value() && av.freq_upper_edge.has_value())
					boxed++;
			}
			if(!rec.annotations.empty())
				printf(" boxed=%zu/%zu", boxed, rec.annotations.size());

			printf("\n");
		}
		catch(const std::exception& e)
		{
			printf("THREW: %s\n", e.what());
		}
	}
	return 0;
}
