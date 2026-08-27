//###############################################################################
//   test_narrow_cfvalue_replace.cpp -- PROTOTYPE ONLY, does not touch
//   production code. See BUILD_NOTES.md section 47/48 and the session
//   plan for context.
//
//   DeepStack's continual re-solving (Moravcik et al., Science 2017,
//   arXiv:1701.01724) does NOT narrow an opponent-range probability
//   vector across streets at all. Quoting the paper directly: "After
//   each action... (iii) Opponent action: no change to our range or
//   the opponent values are required." Instead it carries forward a
//   vector of OPPONENT COUNTERFACTUAL VALUES that is REPLACED (never
//   multiplied) by the freshest re-solve's own output, and each
//   re-solve is conditioned only on the AGENT's own range (narrowed via
//   Bayes' rule on the agent's OWN actions) plus the previous street's
//   opponent counterfactual values -- never on a chain of independently
//   re-solved, already-narrowed opponent-range estimates feeding into
//   the next street's resolve.
//
//   A full, faithful port of that scheme needs an opponent-
//   counterfactual-value vector threaded between streets, which this
//   codebase's LiveResolver/dh_native_ai.cpp architecture does not
//   currently maintain at all (only a WeightedHand villain_range
//   probability vector) -- building that from scratch is out of scope
//   for a same-day prototype and would be a genuine architecture
//   change. This file instead tests the SPECIFIC, most testable piece
//   of DeepStack's spirit that fits the existing plumbing:
//
//     PRODUCTION today: each street's narrowing resolve is SEEDED with
//     the previous street's ALREADY-NARROWED villain_range weights (fed
//     in as `external_reach` -- see narrow_villain_range_postflop() and
//     run_until_converged()'s doc comment in dh_native_ai.cpp). This
//     means each street's computed action-probabilities (avg[idx]) are
//     conditioned on an opponent range that earlier streets' *own*
//     approximate resolves already distorted -- estimation error can
//     compound onto itself, not just genuine signal.
//
//     THIS PROTOTYPE: seed every street's narrowing resolve with a
//     FRESH, UNDISTORTED reach (flat 1.0/hand, only zeroing hands that
//     collide with the board -- i.e. pass nullptr for external_reach,
//     LiveResolver::run()'s own documented default) instead of the
//     running narrowed weights. Each street's avg[idx] is therefore
//     computed independently, decoupled from prior streets' narrowing
//     error, before being multiplied into the tracked belief for
//     reporting -- approximating "replace this street's contribution
//     with a fresh, consistent computation" rather than "chain an
//     already-narrowed estimate forward as if it were ground truth".
//     This is an explicit, scoped APPROXIMATION of DeepStack's actual
//     mechanism, not a full counterfactual-value port -- labeled as
//     such throughout.
//
//   Preflop narrowing is untouched here (calls the real, unmodified
//   narrow_villain_range_preflop() from dh_native_ai.cpp): that
//   mechanism is a direct blueprint-cluster-table lookup, independent
//   of any previously-narrowed state, so it isn't subject to the
//   chained-resolve-input concern this prototype targets.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_narrow_cfvalue_replace tools/test_narrow_cfvalue_replace.cpp
//   RUN (from PokerAI/):
//     ./tools/test_narrow_cfvalue_replace
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

// Verbatim copy of narrow_villain_range_postflop() (dh_native_ai.cpp line
// ~794) with ONE change: run_until_converged() is always called with
// nullptr/nullptr (a fresh, undistorted flat reach) instead of the running
// tracked_weights -- see the file header comment above for why.
void narrow_villain_range_postflop_freshprior(int opp_slot, unsigned char observed_byte) {
	if (g.villain_range.empty()) return;
	if (observed_byte != 'd' && observed_byte != 'l' && observed_byte != 2 && observed_byte != 'n') return;
	try {
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
		std::vector<double> tracked_weights;
		tracked_weights.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) tracked_weights.push_back(h.weight);
		// <-- ONLY CHANGE vs. production: nullptr/nullptr (fresh flat
		// reach) instead of &tracked_weights on whichever side is villain,
		// so this street's resolve isn't conditioned on the accumulated
		// (possibly already-distorted) narrowed belief from prior streets.
		run_until_converged(resolver, mode, nullptr, nullptr);
		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

		double sum = 0.0;
		for (size_t i = 0; i < g.villain_range.size(); i++) {
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
			g.villain_range[i].weight *= avg[idx];
			sum += g.villain_range[i].weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("villain range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : g.villain_range) h.weight /= sum;
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "[CFV-PROTOTYPE] postflop narrowing failed (%s) -- range left unchanged\n", e.what());
	}
}

// Minimal stand-in for opp_take_action(), identical to production except
// it calls narrow_villain_range_postflop_freshprior() instead of the real
// narrow_villain_range_postflop() (preflop narrowing is untouched -- calls
// the real, unmodified production function).
static void opp_take_action_freshprior(const char* actionstr_c) {
	std::string a(actionstr_c);
	int opp = 1 - g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	bool preflop = (g.betting_stage == 0);
	if (a == "allin") {
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('n'); }
		else narrow_villain_range_postflop_freshprior(opp, 'n');
		g.stack[opp] = 0;
		g.has_allin = true;
		int amount = g.stack_at_street_start[opp];
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('n');
	}
	else if (a.rfind("raise ", 0) == 0) {
		int amount = std::stoi(a.substr(6));
		if (preflop && g.preflop_path_confident) {
			int total_pot_before = (20000 - g.stack[0]) + (20000 - g.stack[1]);
			int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
			int my_bet_before = 20000 - g.stack[opp];
			int byte = match_raise_action_byte(total_pot_before, last_bigbet_before, my_bet_before, amount);
			if (byte >= 0) {
				narrow_villain_range_preflop((unsigned char)byte);
				g.preflop_action_path.push_back((unsigned char)byte);
			}
			else g.preflop_path_confident = false;
		}
		else if (!preflop) {
			bool would_be_allin = (street_relative_raise_baseline(opp) - amount) == 0;
			narrow_villain_range_postflop_freshprior(opp, would_be_allin ? (unsigned char)'n' : (unsigned char)2);
		}
		g.stack[opp] = street_relative_raise_baseline(opp) - amount;
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else { // "call" / "check"
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('l'); }
		else narrow_villain_range_postflop_freshprior(opp, 'l');
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
		g.stack[opp] = 20000 - last_bigbet_before;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('l');
	}
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

// Sanity check: the Ac9c/Ad3d fold hand (BUILD_NOTES.md section 47) was
// already confirmed correct under PRODUCTION narrowing (Ad3d ranked
// 95/990, "within expected range", not a miss). Replay it here to check
// the fresh-prior variant doesn't artificially disturb an already-sane
// result. Hero is client_pos=0 this hand; villain (slot 1) opens
// preflop, then raises flop/turn/river.
static void run_ac9c_sanity() {
	int hero_c1 = card_id("Ac"), hero_c2 = card_id("9c");
	int villain_c1 = card_id("Ad"), villain_c2 = card_id("3d");

	std::printf("\n=== Ac9c sanity check (fresh-prior) ===\n");
	restart_game(0, hero_c1, hero_c2); // hero is client_pos=0
	opp_take_action_freshprior("raise 200"); // villain opens to 200
	apply_own_action("call");
	report_combo("[pf] call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("As"), (unsigned char)card_id("9d"), (unsigned char)card_id("8d") };
	Next_stage(1, (char*)flop);
	apply_own_action("raise 200"); // hero bets flop
	opp_take_action_freshprior("raise 1000"); // villain raises to 1000
	apply_own_action("call");
	report_combo("[fl] raise", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("8c") };
	Next_stage(2, (char*)turn);
	apply_own_action("call"); // hero checks
	opp_take_action_freshprior("raise 1200"); // villain bets 1200
	apply_own_action("call");
	report_combo("[tn] raise", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("7h") };
	Next_stage(3, (char*)river);
	apply_own_action("call"); // hero checks
	opp_take_action_freshprior("raise 4800"); // villain bets 4800
	report_combo("[rv] FINAL (real fold decision point)", (unsigned char)villain_c1, (unsigned char)villain_c2);
}

int main() {
	int hero_c1 = card_id("Ad"), hero_c2 = card_id("3c");
	int villain_c1 = card_id("Qh"), villain_c2 = card_id("7s");

	std::printf("Qh7s fresh-prior (replace-not-chain) narrowing prototype (hero=Ad3c).\n"
		"Compare against production numbers in BUILD_NOTES.md section 47:\n"
		"  [1] 0.0557%% rank 939  [2] 0.00248%% rank 874  [2b] 0.00009%% rank 891\n"
		"  [3] 0.00002%% rank 854  [4] ~0.000000%% rank 820  [5] 0.000097%% rank 820/990\n\n");

	restart_game(1, hero_c1, hero_c2);
	report_combo("[0] preflop", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 200");
	opp_take_action_freshprior("call");
	report_combo("[1] preflop call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("Qd"), (unsigned char)card_id("4c"), (unsigned char)card_id("2s") };
	Next_stage(1, (char*)flop);
	opp_take_action_freshprior("call"); // check
	report_combo("[2] flop check", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 400");
	opp_take_action_freshprior("call");
	report_combo("[2b] flop call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("Js") };
	Next_stage(2, (char*)turn);
	opp_take_action_freshprior("call"); // check
	report_combo("[3] turn check", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("call"); // hero checks back

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("Qc") };
	Next_stage(3, (char*)river);
	opp_take_action_freshprior("call"); // check
	report_combo("[4] river check", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("allin");
	opp_take_action_freshprior("call"); // villain calls the shove
	report_combo("[5] FINAL", (unsigned char)villain_c1, (unsigned char)villain_c2);

	std::printf("\n\nAc9c sanity check. Production reference: Ad3d ranked 95/990, "
		"weight 0.1789%% (\"within expected range\", NOT a miss).\n");
	run_ac9c_sanity();

	return 0;
}
