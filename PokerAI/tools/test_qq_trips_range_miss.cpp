//###############################################################################
//   test_qq_trips_range_miss.cpp -- REAL reproduction of a second
//   catastrophic river all-in loss (-20,000 chips), flagged by the user
//   directly from a live session line. Hero (Ad3c, pure ace-high air) bet/
//   barrelled/shoved into a board that paired queens on the river
//   (Qd 4c 2s Js Qc); villain check-called every street and finally called
//   the river shove holding Qh7s -- trip queens. The hand-end diagnostic
//   reported this as one of the worst misses on record: rank 610/990,
//   weight 0.0004% vs. a 0.1010% uniform baseline.
//
//   Real hand sequence:
//     Hero (Ad3c) opens preflop to 200, villain calls.
//     Flop Qd 4c 2s: villain checks, hero bets 400, villain calls.
//     Turn Js: villain checks, hero checks back.
//     River Qc (board pairs queens): villain checks, hero shoves all-in,
//     villain calls -- shows up with Qh7s (trip queens).
//
//   This tool replays that exact sequence through the real production
//   functions (via dh_native_ai.cpp's own opp_take_action()/
//   apply_own_action(), not a reimplementation) and prints Qh7s's tracked
//   weight/rank after EVERY narrowing step, plus a same-step comparison
//   against the OTHER remaining trip-queen combo (Qs-x) and the model's
//   own top-ranked combo, to see exactly where/why trips got buried.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_qq_trips_range_miss tools/test_qq_trips_range_miss.cpp
//   RUN (from PokerAI/):
//     ./tools/test_qq_trips_range_miss
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
	std::printf("%-52s weight=%.6f%% rank=%d/%zu (uniform=%.4f%%) %s\n",
		label, target * 100.0, rank, w.size(), uniform_w * 100.0,
		(target < uniform_w) ? "-- BELOW uniform" : "-- at/above uniform");
}

static void report_top(const char* label, int topn) {
	std::vector<size_t> idx(g.villain_range.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
		return g.villain_range[a].weight > g.villain_range[b].weight;
	});
	std::printf("%s top-%d:", label, topn);
	for (int k = 0; k < topn && k < (int)idx.size(); k++) {
		auto& h = g.villain_range[idx[k]];
		std::printf(" %s%s=%.3f%%", dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0);
	}
	std::printf("\n");
}

int main() {
	int hero_c1 = card_id("Ad"), hero_c2 = card_id("3c");
	int villain_c1 = card_id("Qh"), villain_c2 = card_id("7s");
	// The other remaining trip-queen combo once Qd/Qc are on board and Qh is
	// villain's actual hole card: only Qs is left. Track a representative
	// Qs-x combo too (Qs with the same 7s kicker card is illegal -- 7s is
	// villain's own card in the real deal, but as a DIFFERENT tracked combo
	// in the belief distribution it's a legal, distinct combo) to see if
	// trip queens in general are underweighted, or just this exact kicker.
	int qs_kicker_c1 = card_id("Qs"), qs_kicker_c2 = card_id("7h");

	std::printf("hero=Ad3c (ids %d,%d)  villain actual=Qh7s (ids %d,%d)\n\n",
		hero_c1, hero_c2, villain_c1, villain_c2);

	restart_game(1, hero_c1, hero_c2); // hero is client_pos=1, matching the real hand
	report_combo("[0] fresh preflop prior (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 200"); // hero opens preflop to 200
	std::printf("\n-- villain calls preflop (narrow_villain_range_preflop) --\n");
	opp_take_action((char*)"call");
	report_combo("[1] after preflop call (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("Qd"), (unsigned char)card_id("4c"), (unsigned char)card_id("2s") };
	Next_stage(1, (char*)flop);
	std::printf("\n-- villain checks flop Qd 4c 2s (FLOP mode narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[2] after villain FLOP check (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[2]", 5);

	apply_own_action("raise 400"); // hero bets 400 on flop
	std::printf("\n-- villain calls hero's flop bet --\n");
	opp_take_action((char*)"call");
	report_combo("[2b] after villain FLOP call of bet (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[2b]", 5);

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("Js") };
	Next_stage(2, (char*)turn);
	std::printf("\n-- villain checks turn Js (TURN mode narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[3] after villain TURN check (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[3]", 5);

	apply_own_action("call"); // hero checks back turn
	std::printf("\n-- hero checks back turn (no further narrowing this action) --\n");

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("Qc") };
	Next_stage(3, (char*)river);
	std::printf("\n-- villain checks river Qc -- board PAIRS QUEENS (RIVER mode narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[4] after villain RIVER check, board pairs Q (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_combo("[4] same step, other remaining trip-Q combo (Qs7h):", (unsigned char)qs_kicker_c1, (unsigned char)qs_kicker_c2);
	report_top("[4]", 8);

	apply_own_action("allin"); // hero shoves the river
	std::printf("\n-- villain calls hero's river all-in shove (FINAL) --\n");
	opp_take_action((char*)"call");
	report_combo("[5] FINAL after villain calls river shove (Qh7s):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_combo("[5] same step, other remaining trip-Q combo (Qs7h):", (unsigned char)qs_kicker_c1, (unsigned char)qs_kicker_c2);
	report_top("[5]", 8);

	return 0;
}
