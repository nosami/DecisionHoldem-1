//###############################################################################
//   test_villain_weight_distribution.cpp -- REAL measurement (not assumed)
//   of how concentrated/skewed g.villain_range's weights actually become
//   after real Bayesian narrowing, using the actual production functions
//   (narrow_villain_range_postflop(), which runs a real LiveResolver FLOP
//   best-response) rather than a synthetic uniform test scenario.
//
//   This directly answers: "I'm also assuming that many of the weights
//   would be close to zero" -- with real numbers from the real narrowing
//   code, not a guess.
//
//   Drives the exact same extern "C" entry points the live Slumbot Python
//   server calls (restart_game / Next_stage / opp_take_action), then
//   inspects g.villain_range directly (this tool #includes dh_native_ai.cpp
//   as a single translation unit -- that file defines no main(), so this is
//   safe and reuses 100% real production code, not a reimplementation).
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_villain_weight_distribution tools/test_villain_weight_distribution.cpp
//   RUN (from PokerAI/):
//     ./tools/test_villain_weight_distribution
//###############################################################################
#include "dh_native_ai.cpp"
#include <algorithm>
#include <cstdio>

static void report_weight_distribution(const char* label) {
	std::vector<double> w;
	w.reserve(g.villain_range.size());
	for (auto& h : g.villain_range) w.push_back(h.weight);
	std::vector<double> sorted_w = w;
	std::sort(sorted_w.rbegin(), sorted_w.rend()); // descending

	double total = 0.0;
	for (double x : w) total += x;
	double uniform_w = 1.0 / (double)w.size();

	// How many combos needed to cover 50%/90%/99% of total probability mass?
	double cum = 0.0;
	int n50 = -1, n90 = -1, n99 = -1;
	for (size_t i = 0; i < sorted_w.size(); i++) {
		cum += sorted_w[i];
		if (n50 < 0 && cum >= 0.50 * total) n50 = (int)i + 1;
		if (n90 < 0 && cum >= 0.90 * total) n90 = (int)i + 1;
		if (n99 < 0 && cum >= 0.99 * total) n99 = (int)i + 1;
	}

	// How many combos have "negligible" weight, defined as < 1% of what a
	// uniform prior would assign to each combo (i.e. this combo is carrying
	// less than 1% of its "fair share" of probability)?
	int n_negligible = 0;
	for (double x : w) if (x < 0.01 * uniform_w) n_negligible++;

	std::printf("--- %s ---\n", label);
	std::printf("  combos tracked:        %zu\n", w.size());
	std::printf("  uniform weight would be: %.8f\n", uniform_w);
	std::printf("  max weight:             %.8f (%.2fx uniform)\n", sorted_w.front(), sorted_w.front() / uniform_w);
	std::printf("  min weight:             %.8f (%.4fx uniform)\n", sorted_w.back(), sorted_w.back() / uniform_w);
	std::printf("  combos needed for 50%% of mass: %d / %zu (%.1f%%)\n", n50, w.size(), 100.0 * n50 / w.size());
	std::printf("  combos needed for 90%% of mass: %d / %zu (%.1f%%)\n", n90, w.size(), 100.0 * n90 / w.size());
	std::printf("  combos needed for 99%% of mass: %d / %zu (%.1f%%)\n", n99, w.size(), 100.0 * n99 / w.size());
	std::printf("  combos with < 1%% of uniform's fair share: %d / %zu (%.1f%%)\n\n",
		n_negligible, w.size(), 100.0 * n_negligible / w.size());
}

int main() {
	// Hero: A-K offsuit (arbitrary but reasonable premium starting hand).
	int hero_c1 = 12, hero_c2 = 25; // rank/suit encoding matches deck[] convention in this file

	// === Scenario A: preflop raise, then a FLOP all-in narrowing round ===
	restart_game(0, hero_c1, hero_c2);
	report_weight_distribution("[A] After restart_game() -- fresh uniform preflop prior");

	// Preflop: villain (opp) open-raises, hero calls (not narrowing hero's
	// OWN action -- narrow_villain_range_preflop() only fires from
	// opp_take_action(), which is what production code calls).
	opp_take_action((char*)"raise 400");
	report_weight_distribution("[A] After villain preflop open-raise to 400 (narrow_villain_range_preflop)");

	char flop[3] = { 5, 18, 33 };
	Next_stage(1, flop);
	report_weight_distribution("[A] After Next_stage(flop) -- board-collision pruning only, no new narrowing yet");

	// NOTE: narrow_villain_range_postflop() only fires for an action that
	// maps onto LiveResolver's REDUCED fold/call/allin action abstraction
	// (see that function's own header comment) -- an arbitrary bet size
	// like "raise 800" has no corresponding tree node and is silently
	// skipped (confirmed live: the [DH_RANGE_MODEL] skip message fires for
	// non-all-in raises). Use "allin" so this actually exercises the real
	// LiveResolver narrowing path, not a no-op.
	std::printf("[A] Running real narrow_villain_range_postflop() via a villain FLOP all-in (real LiveResolver FLOP best-response, ~1s)...\n");
	opp_take_action((char*)"allin");
	report_weight_distribution("[A] After villain FLOP all-in (narrow_villain_range_postflop, REAL LiveResolver best-response)");

	// === Scenario B: independent hand -- villain calls flop (no narrowing
	// signal, since a plain call maps to the 'l' node and DOES narrow, but
	// mildly), then goes all-in on the TURN, to see if concentration
	// compounds further after two real narrowing rounds in the same hand. ===
	restart_game(0, hero_c1, hero_c2);
	opp_take_action((char*)"raise 400");
	Next_stage(1, flop);
	std::printf("[B] Running real narrow_villain_range_postflop() via a villain FLOP call (real LiveResolver FLOP best-response, ~1s)...\n");
	opp_take_action((char*)"call");
	report_weight_distribution("[B] After villain FLOP call (first real postflop narrowing round)");

	char turn[4] = { 5, 18, 33, 41 };
	Next_stage(2, turn);
	std::printf("[B] Running a second real narrow_villain_range_postflop() via a villain TURN all-in (TURN mode, ~10-13s worst case)...\n");
	opp_take_action((char*)"allin");
	report_weight_distribution("[B] After villain TURN all-in (second real LiveResolver narrowing round, compounded)");

	return 0;
}
