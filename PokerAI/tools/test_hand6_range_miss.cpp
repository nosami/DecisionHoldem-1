//###############################################################################
//   test_hand6_range_miss.cpp -- REAL reproduction (not a guess) of the
//   catastrophic -20,000 chip river all-in loss recorded as hand #6 of the
//   live Slumbot session at /tmp/run.log. That hand's own
//   [DH_RANGE_MODEL] diagnostic already reported villain's true holding
//   (Jc2c) as a RANGE MISS at hand-end (rank 350/990, weight 0.0020% vs a
//   0.1010% uniform baseline) -- this tool replays the exact same board,
//   hole cards, and action sequence through the real production functions
//   (narrow_villain_range_postflop() via opp_take_action()/apply_own_action(),
//   not a reimplementation -- this #includes dh_native_ai.cpp directly,
//   which defines no main()) and prints Jc2c's tracked weight/rank after
//   EVERY individual narrowing step, to see exactly which street's checks
//   crushed it.
//
//   Real hand #6 sequence (from /tmp/run.log lines 112-158):
//     Hero (9c7h) opens preflop to 200, villain calls.
//     Flop As Js 2d: villain checks, hero checks.
//     Turn 2h (board pairs again): villain checks, hero bets 400, villain calls.
//     River 7s: villain checks, hero shoves all-in, villain calls -- and
//     shows up with Jc2c: a flop TWO PAIR (jacks and deuces) that turned
//     into a FULL HOUSE (deuces full of jacks) on the turn -- i.e. villain
//     was slowplaying a made monster from the turn onward, not bluffing.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_hand6_range_miss tools/test_hand6_range_miss.cpp
//   RUN (from PokerAI/):
//     ./tools/test_hand6_range_miss
//###############################################################################
#include "dh_native_ai.cpp"
#include <algorithm>
#include <cstdio>

// This file's card-id convention (see dh_card_str()'s own comment):
// id = suit*13 + rank, suits "scdh" (s=0,c=1,d=2,h=3), ranks "23456789TJQKA".
static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}

// Finds villain's tracked entry for a specific (c1,c2) combo (order-
// independent) and reports its weight, rank (1 = highest weight), and how
// that weight compares to what a uniform prior over all tracked combos
// would assign -- same metric dh_log_actual_hand() uses in production.
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
		std::printf("%s: combo not tracked (removed by board/hero collision?)\n", label);
		return;
	}
	std::vector<double> sorted_w = w;
	std::sort(sorted_w.rbegin(), sorted_w.rend());
	double target = w[found_idx];
	int rank = 1;
	for (double x : sorted_w) { if (x > target) rank++; }
	double uniform_w = 1.0 / (double)w.size();
	std::printf("%-45s weight=%.6f%% rank=%d/%zu (uniform=%.4f%%) %s\n",
		label, target * 100.0, rank, w.size(), uniform_w * 100.0,
		(target < uniform_w) ? "-- BELOW uniform" : "-- at/above uniform");
}

int main() {
	int hero_c1 = card_id("9c"), hero_c2 = card_id("7h");
	int villain_c1 = card_id("Jc"), villain_c2 = card_id("2c");
	std::printf("hero=9c7h (ids %d,%d)  villain actual=Jc2c (ids %d,%d)\n\n",
		hero_c1, hero_c2, villain_c1, villain_c2);

	restart_game(1, hero_c1, hero_c2); // hero is client_pos=1, matching the real hand
	report_combo("[0] fresh preflop prior:", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 200"); // hero opens preflop to 200
	std::printf("\n-- villain calls preflop (narrow_villain_range_preflop) --\n");
	opp_take_action((char*)"call");
	report_combo("[1] after preflop call:", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("As"), (unsigned char)card_id("Js"), (unsigned char)card_id("2d") };
	Next_stage(1, (char*)flop);
	std::printf("\n-- villain checks flop As Js 2d (FLOP mode narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[2] after villain FLOP check:", (unsigned char)villain_c1, (unsigned char)villain_c2);
	apply_own_action("call"); // hero checks back

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("2h") };
	Next_stage(2, (char*)turn);
	std::printf("\n-- villain checks turn 2h -- board pairs again, villain now has a FULL HOUSE (TURN mode narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[3] after villain TURN check (opening):", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 400"); // hero bets 400 on turn
	std::printf("\n-- villain calls hero's turn bet (2nd TURN mode narrowing) --\n");
	opp_take_action((char*)"call");
	report_combo("[4] after villain TURN call (of hero's bet):", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("7s") };
	Next_stage(3, (char*)river);
	std::printf("\n-- villain checks river 7s (RIVER mode narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[5] after villain RIVER check (opening):", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("allin"); // hero shoves the river
	std::printf("\n-- villain calls hero's river all-in shove (2nd/final RIVER mode narrowing) --\n");
	opp_take_action((char*)"call");
	report_combo("[6] after villain RIVER call (of hero's shove) -- FINAL:", (unsigned char)villain_c1, (unsigned char)villain_c2);

	return 0;
}
