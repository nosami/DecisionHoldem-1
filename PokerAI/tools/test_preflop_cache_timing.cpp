//###############################################################################
//   test_preflop_cache_timing.cpp -- NEW, standalone timing benchmark tool
//   (not part of the shared library).
//
//   Measures the actual before/after per-lookup cost this feature changes:
//   BlueprintReader's direct disk-walk lookup vs. PreflopCache's in-memory
//   lookup, for the SAME real action paths (pulled straight from the cache
//   file, so guaranteed to exist in the trained tree). Existing
//   BUILD_NOTES.md section 23 numbers (6-10s per disk-walk lookup) were
//   measured before this feature existed; this tool re-confirms that
//   number and directly measures the new in-memory path for comparison.
//
//   Build:
//     cd PokerAI && g++ -std=c++17 -O2 -o tools/test_preflop_cache_timing tools/test_preflop_cache_timing.cpp
//   Run (from PokerAI/):
//     ./tools/test_preflop_cache_timing
//###############################################################################
#include "../tree/BlueprintReader.h"
#include "../tree/PreflopCache.h"
#include <iostream>
#include <chrono>
#include <vector>

int main() {
	PreflopCache::Cache cache;
	cache.load("cluster/preflop_blueprint_cache.bin");
	std::cout << "Loaded cache: " << cache.nodes.size() << " nodes\n\n";

	// Pick a handful of real paths at increasing depth (deeper paths are
	// where disk-walk cost is worst, since more sibling subtrees must be
	// skipped to reach them).
	std::vector<std::vector<unsigned char>> paths;
	paths.push_back({}); // root
	for (size_t want_depth = 1; want_depth <= 5; want_depth++) {
		for (const auto& kv : cache.nodes) {
			if (kv.first.size() == want_depth) {
				paths.emplace_back(kv.first.begin(), kv.first.end());
				break;
			}
		}
	}

	std::cout << "path_depth  disk_walk_ms  cache_lookup_ms  speedup\n";
	std::cout << "----------  ------------  ---------------  -------\n";

	for (const auto& path : paths) {
		// Disk-walk timing (single call -- these are slow, no need to repeat).
		auto d0 = std::chrono::steady_clock::now();
		BlueprintReader::AllClustersResult disk_res;
		try {
			disk_res = BlueprintReader::lookup_preflop_strategy_all_clusters(
				"cluster/blueprint_strategy.dat", path);
		} catch (const std::exception& e) {
			std::cerr << "disk-walk failed at depth " << path.size() << ": " << e.what() << "\n";
			continue;
		}
		auto d1 = std::chrono::steady_clock::now();
		double disk_ms = std::chrono::duration<double, std::milli>(d1 - d0).count();

		// Cache timing (repeat many times -- these are fast, need repetition
		// for a stable measurement above clock-resolution noise).
		const int REPS = 10000;
		auto c0 = std::chrono::steady_clock::now();
		for (int i = 0; i < REPS; i++) {
			auto cached_res = PreflopCache::lookup_preflop_strategy_all_clusters(cache, path);
			(void)cached_res;
		}
		auto c1 = std::chrono::steady_clock::now();
		double cache_ms_total = std::chrono::duration<double, std::milli>(c1 - c0).count();
		double cache_ms_avg = cache_ms_total / REPS;

		std::printf("%10zu  %12.1f  %15.6f  %6.0fx\n",
			path.size(), disk_ms, cache_ms_avg, disk_ms / cache_ms_avg);
	}

	return 0;
}
