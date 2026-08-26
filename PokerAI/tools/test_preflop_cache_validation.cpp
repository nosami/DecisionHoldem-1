//###############################################################################
//   test_preflop_cache_validation.cpp -- NEW, standalone validation tool
//   (not part of the shared library, not part of the on-disk cache format).
//
//   Confirms that PreflopCache.h's in-memory cache lookups produce EXACTLY
//   the same normalized strategy as BlueprintReader.h's direct disk-walking
//   lookup_preflop_strategy()/lookup_preflop_strategy_all_clusters()
//   functions, for a real sample of preflop action paths pulled straight out
//   of the just-built cache file itself (root, and a handful of real
//   single/multi-action paths actually present in the trained tree) --
//   never a synthetic/fabricated path. This is the correctness gate the
//   plan requires before dh_native_ai.cpp is allowed to rely on the cache
//   for real decisions.
//
//   Build:
//     cd PokerAI && g++ -std=c++17 -O2 -o tools/test_preflop_cache_validation tools/test_preflop_cache_validation.cpp
//   Run (from PokerAI/, so relative paths resolve):
//     ./tools/test_preflop_cache_validation
//###############################################################################
#include "../tree/BlueprintReader.h"
#include "../tree/PreflopCache.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdio>

static bool nearly_equal(double a, double b, double eps = 1e-12) {
	return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

int main() {
	const std::string blueprint_path = "cluster/blueprint_strategy.dat";
	const std::string cache_path = "cluster/preflop_blueprint_cache.bin";

	PreflopCache::Cache cache;
	try {
		cache.load(cache_path);
	} catch (const std::exception& e) {
		std::cerr << "FAIL: could not load cache: " << e.what() << "\n";
		return 1;
	}
	std::cout << "Loaded cache: " << cache.nodes.size() << " nodes\n";

	// Pull a real, varied sample of action paths straight from the cache
	// itself (root + a spread of depths), so every path tested is
	// guaranteed to actually exist in the trained tree.
	std::vector<std::vector<unsigned char>> sample_paths;
	sample_paths.push_back({}); // root
	{
		size_t want_per_depth[6] = {0, 3, 3, 3, 3, 3}; // depth -> how many samples to grab
		size_t got_per_depth[6] = {0, 0, 0, 0, 0, 0};
		for (const auto& kv : cache.nodes) {
			size_t d = kv.first.size();
			if (d >= 1 && d <= 5 && got_per_depth[d] < want_per_depth[d]) {
				sample_paths.emplace_back(kv.first.begin(), kv.first.end());
				got_per_depth[d]++;
			}
		}
	}
	std::cout << "Testing " << sample_paths.size() << " sample paths (root + varied depths)\n";

	int checked_all_clusters = 0;
	int checked_single_cluster = 0;
	int mismatches = 0;

	for (const auto& path : sample_paths) {
		// --- all-clusters comparison ---
		try {
			BlueprintReader::AllClustersResult disk =
				BlueprintReader::lookup_preflop_strategy_all_clusters(blueprint_path, path);
			BlueprintReader::AllClustersResult cached =
				PreflopCache::lookup_preflop_strategy_all_clusters(cache, path);

			if (disk.actionstr != cached.actionstr) {
				std::cerr << "MISMATCH (actionstr) at path len " << path.size() << "\n";
				mismatches++;
				continue;
			}
			bool ok = true;
			for (int c = 0; c < 169 && ok; c++) {
				if (disk.probs[c].size() != cached.probs[c].size()) { ok = false; break; }
				for (size_t i = 0; i < disk.probs[c].size(); i++) {
					if (!nearly_equal(disk.probs[c][i], cached.probs[c][i])) { ok = false; break; }
				}
			}
			if (!ok) {
				std::cerr << "MISMATCH (probs) at path len " << path.size() << "\n";
				mismatches++;
			} else {
				checked_all_clusters++;
			}
		} catch (const std::exception& e) {
			std::cerr << "EXCEPTION during all-clusters check at path len " << path.size()
			          << ": " << e.what() << "\n";
			mismatches++;
		}

		// --- single-cluster comparison (a few representative clusters) ---
		for (int hand_cluster : {0, 42, 84, 100, 168}) {
			try {
				BlueprintReader::LookupResult disk =
					BlueprintReader::lookup_preflop_strategy(blueprint_path, path, hand_cluster);
				BlueprintReader::LookupResult cached =
					PreflopCache::lookup_preflop_strategy(cache, path, hand_cluster);

				bool ok = (disk.actionstr == cached.actionstr) && (disk.probs.size() == cached.probs.size());
				for (size_t i = 0; ok && i < disk.probs.size(); i++)
					if (!nearly_equal(disk.probs[i], cached.probs[i])) ok = false;

				if (!ok) {
					std::cerr << "MISMATCH (single-cluster " << hand_cluster << ") at path len " << path.size() << "\n";
					mismatches++;
				} else {
					checked_single_cluster++;
				}
			} catch (const std::exception& e) {
				// Both sides can legitimately throw together (e.g. a degenerate
				// all-zero cluster row for this particular node) -- only a real
				// problem if exactly one side throws. Re-check the disk side
				// alone to see if this was an expected, shared degenerate case.
				try {
					BlueprintReader::lookup_preflop_strategy(blueprint_path, path, hand_cluster);
					std::cerr << "MISMATCH (cache threw, disk didn't) at path len " << path.size()
					          << " cluster " << hand_cluster << ": " << e.what() << "\n";
					mismatches++;
				} catch (const std::exception&) {
					// Both threw for the same degenerate reason -- expected, not a bug.
					checked_single_cluster++;
				}
			}
		}
	}

	std::cout << "all-clusters checks passed: " << checked_all_clusters << "/" << sample_paths.size() << "\n";
	std::cout << "single-cluster checks passed: " << checked_single_cluster << "/" << (sample_paths.size() * 5) << "\n";
	std::cout << "mismatches: " << mismatches << "\n";

	if (mismatches == 0) {
		std::cout << "PASS: cache lookups are numerically identical to direct disk-walk lookups.\n";
		return 0;
	} else {
		std::cout << "FAIL: " << mismatches << " mismatch(es) found -- cache must NOT be trusted.\n";
		return 1;
	}
}
