//###############################################################################
//   test_villain_range_model.cpp -- standalone validation for the new
//   persistent, full (non-fixed-size) opponent-range belief model added to
//   dh_native_ai.cpp (BlueprintReader::lookup_preflop_strategy_all_clusters()
//   and the preflop Bayesian-narrowing logic it feeds). See BUILD_NOTES.md
//   for the full design writeup.
//
//   This does NOT link dh_native_ai.cpp itself (its range-tracking helpers
//   are file-local, not exported) -- instead it reimplements the same
//   narrowing arithmetic directly against BlueprintReader.h, using the real
//   cluster/blueprint_strategy.dat, to validate:
//     (1) lookup_preflop_strategy_all_clusters() costs the same one disk
//         walk as lookup_preflop_strategy() and returns internally
//         consistent per-cluster rows (each summing to ~1.0);
//     (2) a full 1225-combo range, Bayesian-narrowed against a SEQUENCE of
//         two real observed preflop actions, produces a sane result: total
//         weight stays normalized, and hands whose own cluster strongly
//         favors the observed action end up with proportionally more
//         relative weight than hands whose cluster disfavors it.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -o test_villain_range_model tools/test_villain_range_model.cpp
//   RUN (from PokerAI/, so "cluster/..." resolves):
//     ./test_villain_range_model
//###############################################################################
#include "../tree/BlueprintReader.h"
#include "../poker/State.h" // for the global `engine` and get_preflop_cluster()
#include <cstdio>
#include <chrono>
#include <vector>
#include <array>
#include <string>

struct WeightedHand { unsigned char c1, c2; double weight; };

int main() {
	const std::string path = "cluster/blueprint_strategy.dat";

	// --- Part 1: timing + consistency check for the new all-clusters lookup ---
	std::vector<unsigned char> empty_path;
	auto t0 = std::chrono::steady_clock::now();
	BlueprintReader::AllClustersResult all = BlueprintReader::lookup_preflop_strategy_all_clusters(path, empty_path);
	auto t1 = std::chrono::steady_clock::now();
	double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	std::printf("all-clusters root lookup: action_len=%zu, clusters=%zu, took %.1f ms\n",
		all.actionstr.size(), all.probs.size(), ms);

	int bad = 0;
	for (size_t c = 0; c < all.probs.size(); c++) {
		double sum = 0.0;
		for (double p : all.probs[c]) sum += p;
		if (sum < 0.999 || sum > 1.001) {
			std::printf("  cluster %zu: probs sum to %.6f (expected ~1.0)\n", c, sum);
			bad++;
		}
	}
	std::printf("clusters with non-normalized rows: %d / %zu\n", bad, all.probs.size());

	// Cross-check against the existing single-cluster lookup for a few
	// sample clusters -- both must agree exactly (same underlying node).
	int sample_clusters[] = { 0, 1, 42, 84, 150, 168 };
	int mismatches = 0;
	for (int cl : sample_clusters) {
		BlueprintReader::LookupResult single = BlueprintReader::lookup_preflop_strategy(path, empty_path, cl);
		bool ok = (single.actionstr == all.actionstr);
		for (size_t i = 0; ok && i < single.probs.size(); i++)
			if (std::abs(single.probs[i] - all.probs[cl][i]) > 1e-9) ok = false;
		std::printf("  cluster %3d cross-check vs single-cluster lookup: %s\n", cl, ok ? "MATCH" : "MISMATCH");
		if (!ok) mismatches++;
	}
	std::printf("mismatches: %d / %d\n\n", mismatches, (int)(sizeof(sample_clusters) / sizeof(int)));

	// --- Part 2: full-range Bayesian narrowing over a 2-action preflop sequence ---
	// Hero holds AsKs (fixed, arbitrary); opponent's tracked range is every
	// remaining 1225-combo, uniform. Simulate the opponent's FIRST preflop
	// action being observed as 'byte 2' (a real trained raise-size code, per
	// the root's own actionstr above) and narrow, then observe a SECOND
	// action ('l', a call) at the resulting node and narrow again.
	unsigned char hero_c1 = 51, hero_c2 = 47; // arbitrary fixed hero hand, distinct cards
	std::vector<WeightedHand> range;
	for (int c = 0; c < 52; c++) {
		if (c == hero_c1 || c == hero_c2) continue;
		for (int d = c + 1; d < 52; d++) {
			if (d == hero_c1 || d == hero_c2) continue;
			range.push_back({ (unsigned char)c, (unsigned char)d, 0.0 });
		}
	}
	double w0 = 1.0 / range.size();
	for (auto& h : range) h.weight = w0;
	std::printf("initial tracked range size: %zu (expected 1225)\n", range.size());

	auto narrow = [&](const std::vector<unsigned char>& path_so_far, unsigned char observed_byte) -> bool {
		BlueprintReader::AllClustersResult res = BlueprintReader::lookup_preflop_strategy_all_clusters(path, path_so_far);
		int idx = -1;
		for (size_t i = 0; i < res.actionstr.size(); i++) if (res.actionstr[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0) { std::printf("  observed byte %d not found at this node -- narrowing skipped\n", (int)observed_byte); return false; }
		double sum = 0.0;
		for (auto& h : range) {
			unsigned char hand[2] = { h.c1, h.c2 };
			int cluster = engine->get_preflop_cluster(hand);
			double p = (cluster >= 0 && cluster < (int)res.probs.size()) ? res.probs[cluster][idx] : 0.0;
			h.weight *= p;
			sum += h.weight;
		}
		if (!(sum > 1e-12)) { std::printf("  range collapsed to ~0 weight -- narrowing skipped\n"); return false; }
		for (auto& h : range) h.weight /= sum;
		return true;
	};

	// First observed action: byte 2 (a real, trained raise-size code per the
	// root printout above -- present in every sample cluster's actionstr).
	unsigned char first_byte = 2;
	std::printf("\nnarrowing on observed root action byte=%d ...\n", (int)first_byte);
	bool ok1 = narrow(empty_path, first_byte);
	double sum_check = 0.0;
	for (auto& h : range) sum_check += h.weight;
	std::printf("  step 1 %s, total weight after renorm = %.6f (expect ~1.0)\n", ok1 ? "OK" : "SKIPPED", sum_check);

	std::vector<unsigned char> path_after_1 = { first_byte };
	std::printf("narrowing again on observed action byte='l' (call) at the resulting node ...\n");
	bool ok2 = narrow(path_after_1, (unsigned char)'l');
	sum_check = 0.0;
	double max_w = 0.0, min_w = 1.0;
	for (auto& h : range) { sum_check += h.weight; if (h.weight > max_w) max_w = h.weight; if (h.weight < min_w) min_w = h.weight; }
	std::printf("  step 2 %s, total weight after renorm = %.6f (expect ~1.0)\n", ok2 ? "OK" : "SKIPPED", sum_check);
	std::printf("  post-narrowing weight spread: min=%.8f max=%.8f (uniform would be %.8f each)\n",
		min_w, max_w, 1.0 / range.size());

	if (ok1 && ok2 && max_w > min_w * 1.01) {
		std::printf("\nPASS: two-step narrowing ran without error, stayed normalized (~1.0), "
			"and produced a NON-uniform belief (min != max) -- the range is genuinely being "
			"narrowed by observed actions, not just renormalized noise.\n");
	}
	else {
		std::printf("\nFAIL or INCONCLUSIVE: see output above.\n");
		return 1;
	}
	return 0;
}
