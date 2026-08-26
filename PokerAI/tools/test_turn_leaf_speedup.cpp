//###############################################################################
//   test_turn_leaf_speedup.cpp -- measures the REAL wall-clock effect of the
//   new RiverClusterLeafModel-based TURN-mode shortcut (BUILD_NOTES.md
//   section 34), using actual production code (this #includes dh_native_ai.cpp
//   directly, which defines no main()) driven through the real ABI entry
//   points (restart_game/opp_take_action/Next_stage/getdecision) -- not a
//   reimplementation or a synthetic microbenchmark of RiverClusterLeafModel in
//   isolation.
//
//   Runs the IDENTICAL scenario (same hero hand, same preflop/flop history,
//   same TURN board, same villain-range belief at that point -- all
//   deterministic, no RNG dependency in what range/board is reached) TWICE in
//   the same process:
//     1. WITHOUT DH_RIVER_SPLIT_DIR set -- TURN resolves use the ORIGINAL
//        exact chance-node + showdown path (unchanged behavior).
//     2. WITH DH_RIVER_SPLIT_DIR set to the real per-hole-hand split river
//        cluster directory -- TURN resolves use the new
//        RiverClusterLeafModel shortcut instead.
//   For each, times both:
//     (a) getdecision() -- hero's own TURN decision (resolve_decision()).
//     (b) opp_take_action("raise 1800") -- a villain TURN raise that
//         triggers narrow_villain_range_postflop() (the extended-action,
//         4-branch resolver).
//   Reports before/after wall-clock for both, and confirms both runs produce
//   a plausible (non-crashing, valid-string) decision -- this is a
//   performance/plumbing check, not a claim that the leaf-model estimate is
//   numerically identical to an exact showdown (it is an approximation, by
//   design, exactly like FLOP's existing TurnClusterLeafModel).
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_turn_leaf_speedup tools/test_turn_leaf_speedup.cpp
//   RUN (from PokerAI/):
//     ./tools/test_turn_leaf_speedup /Users/jason/dh_local_data/river_cluster_split
//###############################################################################
#include "dh_native_ai.cpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <functional>

static double time_ms(const std::function<void()>& fn) {
	auto t0 = std::chrono::steady_clock::now();
	fn();
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Sets up: hero A-K offsuit, opponent opens 400 preflop, hero (unseen here --
// this harness only drives the OPPONENT's actions plus hero's own
// getdecision() calls) faces it, flop dealt, opponent calls (closing flop
// betting so play proceeds to turn), turn dealt. Leaves the game at a real
// TURN decision point, board = {5,18,33,41}, ready for either a hero
// getdecision() or a fresh opponent action.
static void setup_turn_scenario() {
	int hero_c1 = 12, hero_c2 = 25; // A-K offsuit
	restart_game(0, hero_c1, hero_c2);
	opp_take_action((char*)"raise 400");
	char flop[3] = { 5, 18, 33 };
	Next_stage(1, flop);
	opp_take_action((char*)"call");
	char turn[4] = { 5, 18, 33, 41 };
	Next_stage(2, turn);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <river_cluster_split_dir>\n", argv[0]);
		return 1;
	}
	std::string split_dir = argv[1];

	// --- Phase 1: WITHOUT the leaf model (original exact behavior) ---
	unsetenv("DH_RIVER_SPLIT_DIR");
	setup_turn_scenario();
	char out_buf[20];
	double ms_decision_before = time_ms([&]() { getdecision(out_buf); });
	std::string decision_before(out_buf);
	std::printf("[WITHOUT leaf model] getdecision() (hero TURN decision): %.1f ms -> \"%s\"\n",
		ms_decision_before, decision_before.c_str());

	setup_turn_scenario();
	double ms_narrow_before = time_ms([&]() { opp_take_action((char*)"raise 1800"); });
	std::printf("[WITHOUT leaf model] opp_take_action(\"raise 1800\") (villain TURN raise, "
		"narrowing): %.1f ms\n", ms_narrow_before);

	// --- Phase 2: WITH the leaf model enabled ---
	setenv("DH_RIVER_SPLIT_DIR", split_dir.c_str(), 1);
	setup_turn_scenario();
	double ms_decision_after = time_ms([&]() { getdecision(out_buf); });
	std::string decision_after(out_buf);
	std::printf("[WITH leaf model]    getdecision() (hero TURN decision): %.1f ms -> \"%s\"\n",
		ms_decision_after, decision_after.c_str());

	setup_turn_scenario();
	double ms_narrow_after = time_ms([&]() { opp_take_action((char*)"raise 1800"); });
	std::printf("[WITH leaf model]    opp_take_action(\"raise 1800\") (villain TURN raise, "
		"narrowing): %.1f ms\n", ms_narrow_after);

	bool ok = true;
	if (decision_before != "fold" && decision_before != "call" && decision_before != "allin") {
		std::printf("FAIL: getdecision() without leaf model returned an implausible action \"%s\"\n",
			decision_before.c_str());
		ok = false;
	}
	// WITH the leaf model active, full_ladder is now also enabled for TURN
	// (see RealtimeSearch.h's LiveResolver constructor comment and
	// BUILD_NOTES.md section 37), so hero's own decision can legitimately
	// be a real "raise <chips>" string now, not just fold/call/allin --
	// accept any well-formed "raise <positive integer>" in addition to the
	// original three.
	bool is_valid_raise = decision_after.rfind("raise ", 0) == 0 &&
		decision_after.size() > 6 &&
		std::all_of(decision_after.begin() + 6, decision_after.end(), ::isdigit) &&
		std::stoi(decision_after.substr(6)) > 0;
	if (decision_after != "fold" && decision_after != "call" && decision_after != "allin" && !is_valid_raise) {
		std::printf("FAIL: getdecision() with leaf model returned an implausible action \"%s\"\n",
			decision_after.c_str());
		ok = false;
	}

	std::printf("\n--- Summary ---\n");
	std::printf("hero TURN decision:        %.1f ms -> %.1f ms (%.1fx)\n",
		ms_decision_before, ms_decision_after, ms_decision_before / std::max(ms_decision_after, 0.001));
	std::printf("TURN raise narrowing:      %.1f ms -> %.1f ms (%.1fx)\n",
		ms_narrow_before, ms_narrow_after, ms_narrow_before / std::max(ms_narrow_after, 0.001));

	std::printf(ok ? "\nALL CHECKS PASSED\n" : "\nSOME CHECKS FAILED\n");
	return ok ? 0 : 1;
}
