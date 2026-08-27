//###############################################################################
//   test_narrow_epsilon_floor.cpp -- PROTOTYPE ONLY, does not touch
//   production code. See BUILD_NOTES.md section 47/48 and the session
//   plan for context: this experiment tests whether adding a small
//   probability floor ("epsilon") to the per-street villain-range
//   narrowing multiply prevents the irreversible-compounding failure
//   mode observed on the Qh7s trip-queens hand (test_qq_trips_range_
//   miss.cpp / BUILD_NOTES.md section 47), where a combo's weight is
//   crushed so hard by early, only-modestly-unlikely streets that no
//   later street's strong evidence can pull it back up.
//
//   Production narrowing (dh_native_ai.cpp, UNCHANGED by this file):
//     narrow_villain_range_preflop():  h.weight *= p;
//     narrow_villain_range_postflop(): g.villain_range[i].weight *= avg[idx];
//
//   Experimental variant (defined ONLY in this file, new functions with
//   new names -- narrow_villain_range_preflop_epsilon() / _postflop_
//   epsilon() -- calling the exact same underlying primitives
//   (BlueprintReader/PreflopCache, LiveResolver, average_strategy,
//   Players_range, TurnClusterLeafModel/RiverClusterLeafModel) as the
//   real functions, so the only difference is the floor itself):
//     h.weight *= std::max(p, eps);
//     g.villain_range[i].weight *= std::max(avg[idx], eps);
//
//   This tool never calls, modifies, or #defines-away the production
//   narrow_villain_range_preflop()/narrow_villain_range_postflop() --
//   both remain fully intact and unused by this file except as a
//   reference implementation to diff against.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_narrow_epsilon_floor tools/test_narrow_epsilon_floor.cpp
//   RUN (from PokerAI/):
//     ./tools/test_narrow_epsilon_floor
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

// ---------------------------------------------------------------------------
// Experimental narrowing variants: verbatim copies of the production
// functions (dh_native_ai.cpp lines ~561 and ~794 at the time of writing)
// with ONE line changed in each, per the epsilon-floor idea. Everything
// else -- error handling, renormalization, logging call sites removed
// (this is a quiet prototype) -- mirrors the real functions exactly so the
// comparison isolates just the floor's effect.
// ---------------------------------------------------------------------------

static double g_eps = 1e-3; // overridden per experiment run below

void narrow_villain_range_preflop_epsilon(unsigned char observed_byte) {
	if (!g.preflop_path_confident) return;
	try {
		BlueprintReader::AllClustersResult res;
		bool used_cache = false;
		if (g_preflop_cache_loaded) {
			try {
				res = PreflopCache::lookup_preflop_strategy_all_clusters(g_preflop_cache, g.preflop_action_path);
				used_cache = true;
			} catch (const std::exception&) {}
		}
		if (!used_cache) {
			res = BlueprintReader::lookup_preflop_strategy_all_clusters(
				"cluster/blueprint_strategy.dat", g.preflop_action_path);
		}
		int idx = -1;
		for (size_t i = 0; i < res.actionstr.size(); i++)
			if (res.actionstr[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action byte not found among this node's legal actions");
		double sum = 0.0;
		for (auto& h : g.villain_range) {
			unsigned char hand[2] = { h.c1, h.c2 };
			int cluster = engine->get_preflop_cluster(hand);
			double p = (cluster >= 0 && cluster < (int)res.probs.size()) ? res.probs[cluster][idx] : 0.0;
			h.weight *= std::max(p, g_eps); // <-- ONLY CHANGE vs. production
			sum += h.weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("villain range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : g.villain_range) h.weight /= sum;
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "[EPS-PROTOTYPE] preflop narrowing failed (%s) -- range left unchanged\n", e.what());
	}
}

void narrow_villain_range_postflop_epsilon(int opp_slot, unsigned char observed_byte) {
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
		if (opp_slot == 0) run_until_converged(resolver, mode, &tracked_weights, nullptr);
		else run_until_converged(resolver, mode, nullptr, &tracked_weights);
		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

		double sum = 0.0;
		for (size_t i = 0; i < g.villain_range.size(); i++) {
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
			g.villain_range[i].weight *= std::max(avg[idx], g_eps); // <-- ONLY CHANGE vs. production
			sum += g.villain_range[i].weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("villain range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : g.villain_range) h.weight /= sum;
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "[EPS-PROTOTYPE] postflop narrowing failed (%s) -- range left unchanged\n", e.what());
	}
}

// Minimal stand-in for opp_take_action() (dh_native_ai.cpp line ~1157)
// covering only the action types this specific replayed hand needs
// (check/call, sized raise, allin), calling the _epsilon narrowing
// variants above instead of the production ones. Stack/counter
// bookkeeping is copied verbatim from the real function.
static void opp_take_action_epsilon(const char* actionstr_c) {
	std::string a(actionstr_c);
	int opp = 1 - g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	bool preflop = (g.betting_stage == 0);
	if (a == "allin") {
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop_epsilon('n'); }
		else narrow_villain_range_postflop_epsilon(opp, 'n');
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
				narrow_villain_range_preflop_epsilon((unsigned char)byte);
				g.preflop_action_path.push_back((unsigned char)byte);
			}
			else g.preflop_path_confident = false;
		}
		else if (!preflop) {
			bool would_be_allin = (street_relative_raise_baseline(opp) - amount) == 0;
			narrow_villain_range_postflop_epsilon(opp, would_be_allin ? (unsigned char)'n' : (unsigned char)2);
		}
		g.stack[opp] = street_relative_raise_baseline(opp) - amount;
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else { // "call" / "check"
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop_epsilon('l'); }
		else narrow_villain_range_postflop_epsilon(opp, 'l');
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

// Replays the exact Qh7s hand (same sequence as test_qq_trips_range_miss.cpp)
// once, using opp_take_action_epsilon() with the given floor.
static void run_one(double eps) {
	g_eps = eps;
	int hero_c1 = card_id("Ad"), hero_c2 = card_id("3c");
	int villain_c1 = card_id("Qh"), villain_c2 = card_id("7s");

	std::printf("\n=== eps=%.4g ===\n", eps);
	restart_game(1, hero_c1, hero_c2);
	report_combo("[0] preflop", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 200");
	opp_take_action_epsilon("call");
	report_combo("[1] preflop call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("Qd"), (unsigned char)card_id("4c"), (unsigned char)card_id("2s") };
	Next_stage(1, (char*)flop);
	opp_take_action_epsilon("call"); // check
	report_combo("[2] flop check", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("raise 400");
	opp_take_action_epsilon("call");
	report_combo("[2b] flop call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("Js") };
	Next_stage(2, (char*)turn);
	opp_take_action_epsilon("call"); // check
	report_combo("[3] turn check", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("call"); // hero checks back

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("Qc") };
	Next_stage(3, (char*)river);
	opp_take_action_epsilon("call"); // check
	report_combo("[4] river check", (unsigned char)villain_c1, (unsigned char)villain_c2);

	apply_own_action("allin");
	opp_take_action_epsilon("call"); // villain calls the shove
	report_combo("[5] FINAL", (unsigned char)villain_c1, (unsigned char)villain_c2);
}

// Sanity check: the Ac9c/Ad3d fold hand (BUILD_NOTES.md section 47) was
// already confirmed correct under PRODUCTION narrowing (Ad3d ranked
// 95/990, "within expected range", not a miss). Replay it here to check
// neither epsilon floor artificially disturbs an already-sane result.
// Hero is client_pos=0 this hand; villain (slot 1) opens preflop, then
// raises flop/turn/river -- all replayed as opp_take_action_epsilon().
static void run_ac9c_sanity(double eps) {
	g_eps = eps;
	int hero_c1 = card_id("Ac"), hero_c2 = card_id("9c");
	int villain_c1 = card_id("Ad"), villain_c2 = card_id("3d");

	std::printf("\n=== Ac9c sanity check, eps=%.4g ===\n", eps);
	restart_game(0, hero_c1, hero_c2); // hero is client_pos=0
	opp_take_action_epsilon("raise 200"); // villain opens to 200
	apply_own_action("call");
	report_combo("[pf] call", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char flop[3] = { (unsigned char)card_id("As"), (unsigned char)card_id("9d"), (unsigned char)card_id("8d") };
	Next_stage(1, (char*)flop);
	apply_own_action("raise 200"); // hero bets flop
	opp_take_action_epsilon("raise 1000"); // villain raises to 1000
	apply_own_action("call");
	report_combo("[fl] raise", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("8c") };
	Next_stage(2, (char*)turn);
	apply_own_action("call"); // hero checks
	opp_take_action_epsilon("raise 1200"); // villain bets 1200
	apply_own_action("call");
	report_combo("[tn] raise", (unsigned char)villain_c1, (unsigned char)villain_c2);

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("7h") };
	Next_stage(3, (char*)river);
	apply_own_action("call"); // hero checks
	opp_take_action_epsilon("raise 4800"); // villain bets 4800
	report_combo("[rv] FINAL (real fold decision point)", (unsigned char)villain_c1, (unsigned char)villain_c2);
}

int main() {
	std::printf("Qh7s epsilon-floor narrowing prototype (hero=Ad3c). "
		"Compare against production numbers in BUILD_NOTES.md section 47:\n"
		"  [1] 0.0557%% rank 939  [2] 0.00248%% rank 874  [2b] 0.00009%% rank 891\n"
		"  [3] 0.00002%% rank 854  [4] ~0.000000%% rank 820  [5] 0.000097%% rank 820/990\n");
	run_one(1e-3);
	run_one(1e-2);

	std::printf("\n\nAc9c sanity check. Production reference: Ad3d ranked 95/990, "
		"weight 0.1789%% (\"within expected range\", NOT a miss).\n");
	run_ac9c_sanity(1e-3);
	run_ac9c_sanity(1e-2);
	return 0;
}
