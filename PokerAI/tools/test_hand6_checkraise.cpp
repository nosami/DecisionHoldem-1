//###############################################################################
//   test_hand6_checkraise.cpp -- follow-up to test_hand6_range_miss.cpp,
//   answering: "for checked hands, we also need to consider which hands
//   would check-raise". Does the narrowing resolver's own solved strategy
//   already account for the possibility that villain's hand plans to
//   check-RAISE (not just check-call) if hero bets after the check? And if
//   so, does villain's real holding (Jc2c -- a turned full house) actually
//   carry meaningful check-raise equity in the model's own solve, or does
//   the model's equilibrium simply prefer betting/raising such hands
//   outright (making a check genuinely strong evidence against holding one)?
//
//   This #includes dh_native_ai.cpp directly (real production code, no
//   reimplementation) and manually reconstructs the exact RIVER-mode
//   narrowing resolver narrow_villain_range_postflop() builds internally
//   (same LiveResolver construction, extended_actions=true so a genuine
//   check-raise node -- byte 2, a canonical 1x-pot raise -- exists both at
//   villain's opening decision AND at villain's response after a hero bet),
//   then walks the resolved Node tree directly to read out:
//     (a) villain's own root strategy for Jc2c: P(check) vs P(bet) vs P(allin)
//     (b) CONDITIONAL on checking and hero betting, villain's strategy for
//         Jc2c at that next decision: P(fold) vs P(call) vs P(raise=check-
//         raise!) vs P(allin)
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_hand6_checkraise tools/test_hand6_checkraise.cpp
//   RUN (from PokerAI/):
//     ./tools/test_hand6_checkraise
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

static void print_dist(const char* label, const std::vector<unsigned char>& actions,
	const std::vector<double>& avg) {
	fprintf(stderr, "%s: ", label);
	for (size_t i = 0; i < actions.size(); i++)
		fprintf(stderr, "%s=%.2f%%  ", dh_action_name(actions[i]).c_str(), avg[i] * 100.0);
	fprintf(stderr, "\n");
}

int main() {
	int hero_c1 = card_id("9c"), hero_c2 = card_id("7h");
	int villain_c1 = card_id("Jc"), villain_c2 = card_id("2c");

	// Replay hand #6 up to (but not including) the river's opening check --
	// identical to test_hand6_range_miss.cpp.
	restart_game(1, hero_c1, hero_c2);
	apply_own_action("raise 200");
	opp_take_action((char*)"call");
	unsigned char flop[3] = { (unsigned char)card_id("As"), (unsigned char)card_id("Js"), (unsigned char)card_id("2d") };
	Next_stage(1, (char*)flop);
	opp_take_action((char*)"call"); // villain checks flop
	apply_own_action("call");        // hero checks back
	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("2h") };
	Next_stage(2, (char*)turn);
	opp_take_action((char*)"call");  // villain checks turn (opening)
	apply_own_action("raise 400");   // hero bets 400
	opp_take_action((char*)"call");  // villain calls
	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("7s") };
	Next_stage(3, (char*)river);

	// Find villain's tracked index for Jc2c.
	int vidx = -1;
	for (size_t i = 0; i < g.villain_range.size(); i++) {
		auto& h = g.villain_range[i];
		if ((h.c1 == villain_c1 && h.c2 == villain_c2) || (h.c1 == villain_c2 && h.c2 == villain_c1)) { vidx = (int)i; break; }
	}
	if (vidx < 0) { fprintf(stderr, "Jc2c not tracked (unexpected)\n"); return 1; }
	fprintf(stderr, "Jc2c tracked at villain_range index %d (weight going in: %.6f%%)\n\n",
		vidx, g.villain_range[vidx].weight * 100.0);

	// Manually reconstruct EXACTLY the resolver narrow_villain_range_postflop()
	// builds for the RIVER-check step (opp_slot=0, since hero is my_id=1),
	// but keep it alive afterward so we can walk the tree.
	int opp_slot = 0;
	Searchstate s = build_current_searchstate(opp_slot);
	std::array<unsigned char, 2> my_hand = { g.my_hole[0], g.my_hole[1] };
	std::vector<std::array<unsigned char, 2>> tracked_hands;
	for (auto& h : g.villain_range) tracked_hands.push_back({ h.c1, h.c2 });
	// Must match narrow_villain_range_postflop()'s own swap exactly:
	// Players_range's "hero"/"villain" fields are keyed by internal engine
	// PLAYER SLOT (0 vs 1), not by actual hero/villain identity -- when
	// opp_slot==0 (villain occupies engine slot 0, as here), the tracked
	// (990-combo) range goes in range.hero and hero's own single real hand
	// goes in range.villain. Getting this backwards silently swaps N/M and
	// segfaults on any out-of-range hand_idx (found the hard way).
	Players_range range;
	if (opp_slot == 0) { range.hero = tracked_hands; range.villain = { my_hand }; }
	else { range.hero = { my_hand }; range.villain = tracked_hands; }
	fprintf(stderr, "constructing resolver, hero range size=%zu villain range size=%zu\n", range.hero.size(), range.villain.size());
	LiveResolver resolver(range, engine, nullptr, LiveResolver::Mode::RIVER, /*extended_actions=*/true, nullptr);
	fprintf(stderr, "resolver constructed, calling init_root...\n");
	resolver.init_root(s, g.board);
	fprintf(stderr, "init_root done\n");
	std::vector<double> tracked_weights;
	for (auto& h : g.villain_range) tracked_weights.push_back(h.weight);
	fprintf(stderr, "Running real LiveResolver RIVER resolve (~5-10s)...\n");
	run_until_converged(resolver, LiveResolver::Mode::RIVER, nullptr, &tracked_weights);
	fprintf(stderr, "resolve done. root=%p root->actions.size()=%zu root->children.size()=%zu\n",
		(void*)resolver.root.get(), resolver.root->actions.size(), resolver.root->children.size());

	// (a) Villain's root strategy for Jc2c: P(check) vs P(bet-1x-pot) vs P(allin)
	std::vector<double> root_avg;
	LiveResolver::average_strategy(resolver.root.get(), vidx, root_avg);
	print_dist("[root] villain Jc2c strategy (opening river decision)", resolver.root->actions, root_avg);

	int idx_check = -1;
	for (size_t i = 0; i < resolver.root->actions.size(); i++) if (resolver.root->actions[i] == 'l') idx_check = (int)i;
	if (idx_check < 0) { fprintf(stderr, "no check action at root (unexpected)\n"); return 1; }
	fprintf(stderr, "idx_check=%d children.size()=%zu\n", idx_check, resolver.root->children.size());
	LiveResolver::Node* after_check = resolver.root->children.at(idx_check).get();
	fprintf(stderr, "after_check=%p\n", (void*)after_check);
	if (!after_check || !after_check->expanded) { fprintf(stderr, "check child not expanded -- CFR may not have visited it\n"); return 1; }
	fprintf(stderr, "after_check betting_stage=%d actions.size()=%zu strat_sum.size()=%zu is_chance=%d\n",
		(int)after_check->state.betting_stage, after_check->actions.size(), after_check->strat_sum.size(), (int)after_check->is_chance);

	// Hero (N=1, hand_idx 0) acts next, facing 0 owed.
	std::vector<double> hero_avg;
	LiveResolver::average_strategy(after_check, 0, hero_avg);
	print_dist("[after villain checks] hero's own strategy", after_check->actions, hero_avg);

	int idx_bet = -1;
	for (size_t i = 0; i < after_check->actions.size(); i++) if (after_check->actions[i] == 2) idx_bet = (int)i;
	if (idx_bet < 0) { fprintf(stderr, "no 1x-pot bet action available to hero after check (unexpected)\n"); return 1; }
	fprintf(stderr, "idx_bet=%d after_check->children.size()=%zu\n", idx_bet, after_check->children.size());
	LiveResolver::Node* after_bet = after_check->children.at(idx_bet).get();
	fprintf(stderr, "after_bet=%p\n", (void*)after_bet);
	if (!after_bet || !after_bet->expanded) { fprintf(stderr, "post-bet child not expanded\n"); return 1; }

	// (b) Villain's strategy for Jc2c facing hero's bet AFTER having checked --
	// this is the actual check-RAISE decision point.
	std::vector<double> checkraise_avg;
	LiveResolver::average_strategy(after_bet, vidx, checkraise_avg);
	print_dist("[after villain checks, hero bets] villain Jc2c strategy (check-raise decision)",
		after_bet->actions, checkraise_avg);

	return 0;
}
