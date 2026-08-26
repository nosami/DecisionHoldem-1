//###############################################################################
//   test_blueprint_root_read.cpp -- standalone validation tool for
//   PokerAI/tree/BlueprintReader.h, the new targeted/streaming reader for
//   cluster/blueprint_strategy.dat (see that header's top comment and
//   BUILD_NOTES.md for the full format writeup).
//
//   WHY THIS EXISTS: BlueprintReader.h was written by careful, byte-for-byte
//   inspection of PokerAI/tree/Save_load.h's write-side code, but this
//   development sandbox lacks OS-level disk permission to read
//   cluster/blueprint_strategy.dat (it lives on an external drive this
//   sandbox's process cannot open -- a permission-scoping quirk, not a code
//   defect; see BUILD_NOTES.md section 17). It could NOT be run/validated
//   from within this session. This tool lets the USER (who has working
//   access) validate it in seconds, since a root-only read only touches a
//   few KB of the ~16GB file -- it does NOT load the whole tree.
//
//   WHAT TO LOOK FOR: this reads ONLY the tree's root node (hero's very
//   first preflop decision, no action history), which is shared by all 169
//   preflop hand-cluster buckets, and prints, for a handful of sample
//   clusters, the legal action bytes and the normalized average strategy
//   (probabilities). A sane, real result should show:
//     - action_len small (2-8ish), actionstr bytes matching
//       PokerAI/poker/State.h's codes ('d'=100 fold if a bet is owed,
//       'l'=108 call, 'n'=110 allin, plus small integers like 1,2,4,8,20,40
//       for pot-fraction raise sizes)
//     - probabilities for each sample cluster summing to ~1.0
//     - NOT every cluster producing an identical, degenerate distribution
//       (that would suggest either a corrupt/placeholder file, or that this
//       reader is misinterpreting the format)
//   This tool does NOT judge whether the strategy is "good" poker (169
//   cluster IDs are not intuitively ordered by hand strength) -- it only
//   sanity-checks that the bytes are being parsed the way Save_load.h wrote
//   them.
//
//   BUILD (run from PokerAI/, same convention as the other tools/tests):
//     g++ -std=c++17 -O2 -o test_blueprint_root_read tools/test_blueprint_root_read.cpp
//
//   RUN (from PokerAI/, so the relative "cluster/..." path resolves):
//     ./test_blueprint_root_read
//   Or pass an explicit path:
//     ./test_blueprint_root_read /path/to/blueprint_strategy.dat
//###############################################################################
#include "../tree/BlueprintReader.h"
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
	std::string path = (argc > 1) ? argv[1] : "cluster/blueprint_strategy.dat";
	std::printf("Reading root node from: %s\n", path.c_str());

	std::vector<unsigned char> empty_path; // root = no history yet
	// Sample a handful of cluster IDs spread across the 169-wide range.
	int sample_clusters[] = { 0, 1, 42, 84, 100, 150, 168 };

	for (int cl : sample_clusters) {
		try {
			BlueprintReader::LookupResult res =
				BlueprintReader::lookup_preflop_strategy(path, empty_path, cl);
			std::printf("cluster %3d: action_len=%zu  actions/probs:", cl, res.actionstr.size());
			double sum = 0.0;
			for (size_t i = 0; i < res.actionstr.size(); i++) {
				unsigned char b = res.actionstr[i];
				if (b >= 32 && b < 127)
					std::printf("  '%c'(%d)=%.4f", (char)b, (int)b, res.probs[i]);
				else
					std::printf("  [%d]=%.4f", (int)b, res.probs[i]);
				sum += res.probs[i];
			}
			std::printf("   (sum=%.6f)\n", sum);
		}
		catch (const std::exception& e) {
			std::printf("cluster %3d: FAILED -- %s\n", cl, e.what());
		}
	}
	std::printf(
		"\nIf every cluster above shows a small action_len, sane byte codes, "
		"and probabilities summing to ~1.0 (and NOT every cluster identical), "
		"the reader is very likely correct and dh_native_ai.cpp's preflop "
		"decisions are using the real trained blueprint. Report back what you "
		"see.\n");
	return 0;
}
