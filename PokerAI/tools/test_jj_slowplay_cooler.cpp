//###############################################################################
//   test_jj_slowplay_cooler.cpp -- REAL reproduction of a river all-in loss
//   (-20,000 chips) the user flagged directly from a live session line.
//   Hero (ThTd, an underpair to this board) 3-bet-called preflop, checked
//   flop, barrelled turn and river as a near-pure bluff (the live strategy
//   line showed ~87-99% raise/allin frequency on both streets); villain
//   slowplayed flopped trip jacks the entire way (check flop, check turn,
//   check river) and called both barrels, showing JhJc.
//
//   Real hand sequence (client_pos=1, i.e. hero is the small blind -- see
//   BUILD_NOTES.md's confirmed client_pos<->seat mapping):
//     Hero (ThTd) opens preflop to 300, villain 3-bets to 900, hero calls.
//     Flop Ks Jd 2h (villain already has trip jacks here): check, check.
//     Turn 5h: villain checks, hero bets 1800 (pot), villain calls.
//     River Qd: villain checks, hero shoves 17300, villain calls -- shows
//     JhJc (trip jacks since the flop).
//
//   User's question this tool answers: "did we narrow to the 3-bet range
//   preflop?" -- i.e. after villain's preflop 3-bet, did narrow_villain_
//   range_preflop() actually fire and produce a value-shaped distribution,
//   and where did JJ (a totally standard 3-bet hand) land in it? The live
//   log only printed the top-5 after that step (all unpaired Broadway
//   combos: KcAc/KsAs/KhAh/KdAd/QdAd), not JJ's own rank -- this replays
//   the exact sequence through the real production functions (dh_native_
//   ai.cpp's own opp_take_action()/apply_own_action(), not a
//   reimplementation) and prints JhJc's tracked weight/rank after EVERY
//   narrowing step, so we can see exactly whether/where trip jacks got
//   buried, and whether that's a preflop-narrowing effect, a postflop
//   (check-check-check) effect, or both.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_jj_slowplay_cooler tools/test_jj_slowplay_cooler.cpp
//   RUN (from PokerAI/):
//     ./tools/test_jj_slowplay_cooler
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
	double percentile_from_bottom = 100.0 * (double)(w.size() - rank) / (double)(w.size() - 1);
	std::printf("%-52s weight=%.6f%% rank=%d/%zu (percentile=%.1f%% from bottom, uniform=%.4f%%) %s\n",
		label, target * 100.0, rank, w.size(), percentile_from_bottom, uniform_w * 100.0,
		(target < uniform_w) ? "-- BELOW uniform" : "-- at/above uniform");
}

// Also reports how many distinct pocket-pair combos (any rank, e.g. JJ, QQ,
// 99...) are tracked and what fraction of total weight they hold in
// aggregate, since the user's concern is specifically about whether
// "already-strong-since-early-street" hand *types* (sets/trips from a
// pocket pair matching the board) are structurally underweighted relative
// to "hands that back into strength on a later street" (e.g. two pair made
// on the river). This does NOT assume the answer -- it just measures it.
static void report_pair_mass(const char* label) {
	double pair_mass = 0.0, total = 0.0;
	int pair_combos = 0;
	for (auto& h : g.villain_range) {
		total += h.weight;
		if ((h.c1 % 13) == (h.c2 % 13)) { pair_mass += h.weight; pair_combos++; }
	}
	std::printf("%s pocket-pair combos tracked=%d, aggregate pair weight=%.3f%% of total\n",
		label, pair_combos, 100.0 * pair_mass / total);
}

// Directly probes the RAW per-combo action-probability distribution
// (average_strategy() output) at the exact "villain about to act" node
// narrow_villain_range_postflop() itself resolves, WITHOUT going through
// its aggregate weight-multiply -- i.e. this answers "does villain's own
// solved equilibrium strategy assign JJ (and hands like it) a plausible
// check-raise-consistent P(check), or something pathologically close to
// zero?" This mirrors narrow_villain_range_postflop()'s own resolver setup
// exactly (same opp_slot/searchstate/mode/leaf-model/extended_actions),
// but reads average_strategy() directly per-combo instead of only using it
// to multiply into g.villain_range. Must be called BEFORE opp_take_action()
// applies the real narrowing update for the same observed action (which
// re-resolves an equivalent tree from scratch, so this doubles the CFR
// cost here but leaves the real production call path untouched).
static void probe_check_strategy(const char* label, unsigned char jj_c1, unsigned char jj_c2,
	long long long_run_extra_ms = 0) {
	int opp_slot = 1 - g.my_id;
	Searchstate s = build_current_searchstate(opp_slot);
	std::array<unsigned char, 2> my_hand = { g.my_hole[0], g.my_hole[1] };
	std::vector<std::array<unsigned char, 2>> tracked_hands;
	tracked_hands.reserve(g.villain_range.size());
	for (auto& h : g.villain_range) tracked_hands.push_back({ h.c1, h.c2 });

	Players_range range;
	if (opp_slot == 0) { range.hero = tracked_hands; range.villain = { my_hand }; }
	else { range.hero = { my_hand }; range.villain = tracked_hands; }

	LiveResolver::Mode mode = (g.betting_stage == 1) ? LiveResolver::Mode::FLOP
		: (g.betting_stage == 2) ? LiveResolver::Mode::TURN
		: LiveResolver::Mode::RIVER;
	std::unique_ptr<TurnClusterLeafModel> leaf;
	if (mode == LiveResolver::Mode::FLOP) {
		unsigned char flop_board[3] = { g.board[0], g.board[1], g.board[2] };
		leaf.reset(new TurnClusterLeafModel(engine, flop_board, range));
	}
	std::unique_ptr<RiverClusterLeafModel> river_leaf;
	if (mode == LiveResolver::Mode::TURN) {
		std::string dir = river_split_dir();
		if (!dir.empty()) {
			unsigned char turn_board[4] = { g.board[0], g.board[1], g.board[2], g.board[3] };
			river_leaf.reset(new RiverClusterLeafModel(dir, turn_board, range));
		}
	}
	LiveResolver resolver(range, engine, leaf.get(), mode, /*extended_actions=*/true, river_leaf.get());
	resolver.init_root(s, g.board);

	// Real production budget first (byte-for-byte what narrow_villain_range_postflop
	// would do): FLOP = 3000ms/10000 iters cap. Report exploitability reached.
	auto t0 = std::chrono::steady_clock::now();
	run_until_converged(resolver, mode, nullptr, nullptr);
	double ms_prod = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	double pot = (double)resolver.root->state.table.total_pot;
	double expl_prod = (pot > 1e-9) ? 100.0 * resolver.exploitability(nullptr, nullptr) / pot : 0.0;
	std::printf("%s [production budget] elapsed=%.0fms exploitability=%.2f%% of pot\n", label, ms_prod, expl_prod);

	if (long_run_extra_ms > 0) {
		// Continue accumulating CFR iterations on the SAME persistent tree
		// (regret/strat_sum already validated to accumulate correctly across
		// separate run() calls -- see run_until_converged()'s own comment) for
		// a much bigger extra wall-clock budget, to see whether JJ's P(check)
		// (and overall exploitability) actually moves once given far more time
		// than the real-time production cap allows, or whether it was already
		// at/near its converged value.
		auto t1 = std::chrono::steady_clock::now();
		int extra_iters = 0;
		while (true) {
			resolver.run(200, nullptr, nullptr);
			extra_iters += 200;
			double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
			if (elapsed >= long_run_extra_ms) break;
		}
		double expl_long = (pot > 1e-9) ? 100.0 * resolver.exploitability(nullptr, nullptr) / pot : 0.0;
		std::printf("%s [+%.0fms / %d extra iters] exploitability=%.2f%% of pot\n",
			label, (double)long_run_extra_ms, extra_iters, expl_long);
	}

	int check_idx = -1;
	std::printf("%s root actions available:", label);
	for (size_t i = 0; i < resolver.root->actions.size(); i++) {
		std::printf(" %s", dh_action_name(resolver.root->actions[i]).c_str());
		if (resolver.root->actions[i] == 'l') check_idx = (int)i;
	}
	std::printf("\n");
	if (check_idx < 0) { std::printf("%s: no check/call action at this node\n", label); return; }

	double sum_check = 0.0, jj_check = -1.0;
	double min_check = 2.0, max_check = -1.0;
	int jj_idx = -1;
	std::vector<double> all_check;
	all_check.reserve(g.villain_range.size());
	for (size_t i = 0; i < g.villain_range.size(); i++) {
		std::vector<double> avg;
		LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
		double p_check = avg[check_idx];
		all_check.push_back(p_check);
		sum_check += p_check;
		min_check = std::min(min_check, p_check);
		max_check = std::max(max_check, p_check);
		auto& h = g.villain_range[i];
		if ((h.c1 == jj_c1 && h.c2 == jj_c2) || (h.c1 == jj_c2 && h.c2 == jj_c1)) { jj_check = p_check; jj_idx = (int)i; }
	}
	double mean_check = sum_check / (double)g.villain_range.size();
	std::sort(all_check.begin(), all_check.end());
	double median_check = all_check[all_check.size() / 2];
	std::printf("%s P(check) stats across %zu combos: mean=%.4f median=%.4f min=%.4f max=%.4f\n",
		label, g.villain_range.size(), mean_check, median_check, min_check, max_check);
	if (jj_idx >= 0)
		std::printf("%s JhJc's OWN P(check)=%.4f (rank among all combos' P(check): ", label, jj_check);
	if (jj_idx >= 0) {
		int rank = 1;
		for (double x : all_check) if (x > jj_check) rank++;
		std::printf("%d/%zu, i.e. %s the median)\n", rank, g.villain_range.size(),
			jj_check > median_check ? "ABOVE" : (jj_check < median_check ? "BELOW" : "AT"));
	}
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
	int hero_c1 = card_id("Th"), hero_c2 = card_id("Td");
	int villain_c1 = card_id("Jh"), villain_c2 = card_id("Jc");

	std::printf("hero=ThTd (ids %d,%d)  villain actual=JhJc (ids %d,%d)\n\n",
		hero_c1, hero_c2, villain_c1, villain_c2);

	restart_game(1, hero_c1, hero_c2); // hero is client_pos=1, matching the real hand
	report_combo("[0] fresh preflop prior (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_pair_mass("[0]");

	apply_own_action("raise 300"); // hero opens preflop to 300
	std::printf("\n-- villain 3-bets preflop to 900 (narrow_villain_range_preflop, THE QUESTION) --\n");
	opp_take_action((char*)"raise 900");
	report_combo("[1] after villain's preflop 3-bet (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[1]", 8);
	report_pair_mass("[1]");

	apply_own_action("call"); // hero calls the 3-bet
	std::printf("\n-- hero calls the 3-bet (no further narrowing this action) --\n");

	unsigned char flop[3] = { (unsigned char)card_id("Ks"), (unsigned char)card_id("Jd"), (unsigned char)card_id("2h") };
	Next_stage(1, (char*)flop);
	std::printf("\n-- BEFORE applying the check, probe villain's own raw P(check) per combo --\n");
	std::printf("-- (does the model's own equilibrium give JJ a plausible check-raise-consistent\n");
	std::printf("--  frequency here, or something pathologically near zero?) --\n");
	probe_check_strategy("[2-probe]", (unsigned char)villain_c1, (unsigned char)villain_c2, /*long_run_extra_ms=*/60000);
	std::printf("\n-- villain checks flop Ks Jd 2h -- villain already has trip jacks here (FLOP narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[2] after villain FLOP check (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[2]", 8);
	report_pair_mass("[2]");

	apply_own_action("call"); // hero checks back flop
	std::printf("\n-- hero checks back flop (no further narrowing this action) --\n");

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("5h") };
	Next_stage(2, (char*)turn);
	std::printf("\n-- villain checks turn 5h (TURN narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[3] after villain TURN check (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[3]", 8);
	report_pair_mass("[3]");

	apply_own_action("raise 1800"); // hero barrels turn (pot-size bet)
	std::printf("\n-- villain calls hero's turn barrel --\n");
	opp_take_action((char*)"call");
	report_combo("[3b] after villain TURN call of barrel (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[3b]", 8);
	report_pair_mass("[3b]");

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("Qd") };
	Next_stage(3, (char*)river);
	std::printf("\n-- villain checks river Qd (RIVER narrowing) --\n");
	opp_take_action((char*)"call"); // check
	report_combo("[4] after villain RIVER check (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[4]", 8);
	report_pair_mass("[4]");

	apply_own_action("allin"); // hero shoves the river
	std::printf("\n-- villain calls hero's river all-in shove (FINAL) --\n");
	opp_take_action((char*)"call");
	report_combo("[5] FINAL after villain calls river shove (JhJc):", (unsigned char)villain_c1, (unsigned char)villain_c2);
	report_top("[5]", 8);
	report_pair_mass("[5]");

	return 0;
}
