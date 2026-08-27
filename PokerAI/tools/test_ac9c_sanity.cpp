//###############################################################################
//   test_ac9c_sanity.cpp -- REAL production reproduction (not a prototype
//   copy) of the Ac9c/Ad3d sanity hand from BUILD_NOTES.md sections 47-49.
//   That hand was already confirmed correct under the OLD (chained)
//   narrowing (Ad3d ranked 95/990, "within expected range", not a miss).
//   This tool replays the exact same board/hole cards/action sequence
//   through the real production functions (narrow_villain_range_postflop()
//   via opp_take_action()/apply_own_action() -- this #includes
//   dh_native_ai.cpp directly, which defines no main()) to confirm the
//   fresh-prior narrowing change (section 49) doesn't regress this already-
//   correct result, and to show the expected drop in absolute weight (the
//   reason dh_log_actual_hand()'s miss diagnostic was made rank-based
//   instead of absolute-weight-based -- see section 49).
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_ac9c_sanity tools/test_ac9c_sanity.cpp
//   RUN (from PokerAI/):
//     ./tools/test_ac9c_sanity
//###############################################################################
#include "dh_native_ai.cpp"
#include <algorithm>
#include <cstdio>

static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}

static void report_combo(const char* label, unsigned char c1, unsigned char c2) {
	std::vector<double> w;
	w.reserve(g.villain_range.size());
	int found_idx = -1;
	for (size_t i = 0; i < g.villain_range.size(); i++) {
		auto& h = g.villain_range[i];
		w.push_back(h.weight);
		if ((h.c1 == c1 && h.c2 == c2) || (h.c1 == c2 && h.c2 == c1)) found_idx = (int)i;
	}
	if (found_idx < 0) {
		std::printf("%s: combo not tracked\n", label);
		return;
	}
	std::vector<double> sorted_w = w;
	std::sort(sorted_w.rbegin(), sorted_w.rend());
	double target = w[found_idx];
	int rank = 1;
	for (double x : sorted_w) { if (x > target) rank++; }
	double uniform_w = 1.0 / (double)w.size();
	std::printf("%-14s weight=%9.6f%% rank=%4d/%zu (uniform=%.4f%%)\n",
		label, target * 100.0, rank, w.size(), uniform_w * 100.0);
}

int main() {
	int hero_c1 = card_id("Ac"), hero_c2 = card_id("9c");
	int villain_c1 = card_id("Ad"), villain_c2 = card_id("3d");

	std::printf("Ac9c/Ad3d sanity check against REAL production narrow_villain_range_postflop()\n"
		"(fresh-prior, BUILD_NOTES.md section 49). Reference: production BEFORE\n"
		"this change ranked Ad3d 95/990, weight 0.1789%% (\"within expected range\").\n\n");

	restart_game(0, hero_c1, hero_c2); // hero is client_pos=0
	opp_take_action((char*)"raise 200"); // villain opens to 200
	apply_own_action("call");
	report_combo("[pf] call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("As"), (unsigned char)card_id("9d"), (unsigned char)card_id("8d") };
	Next_stage(1, (char*)flop);
	apply_own_action("raise 200"); // hero bets flop
	opp_take_action((char*)"raise 1000"); // villain raises to 1000
	apply_own_action("call");
	report_combo("[fl] raise", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("8c") };
	Next_stage(2, (char*)turn);
	apply_own_action("call"); // hero checks
	opp_take_action((char*)"raise 1200"); // villain bets 1200
	apply_own_action("call");
	report_combo("[tn] raise", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("7h") };
	Next_stage(3, (char*)river);
	apply_own_action("call"); // hero checks
	opp_take_action((char*)"raise 4800"); // villain bets 4800
	report_combo("[rv] FINAL (real fold decision point)", (unsigned char)villain_c1, (unsigned char)villain_c2);

	dh_log_actual_hand((unsigned char)villain_c1, (unsigned char)villain_c2);

	return 0;
}
