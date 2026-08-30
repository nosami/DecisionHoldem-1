//###############################################################################
//   test_hand_12473147059_texassolver_compare.cpp -- real live SkyPoker hand,
//   replayed through DecisionHoldem's REAL production functions to dump the
//   EXACT g.hero_range/g.villain_range weighted-combo arrays it used for its
//   real river decision, for a TexasSolver "exact same input" comparison.
//   Second hand in this series -- see test_hand_12473146716_texassolver_compare.cpp
//   (hand 1) for the full methodology writeup this file follows exactly.
//
//   Real hand (game_logs/hand_12473147059/hand_history.txt):
//     Hero (nosami, BB) dealt 6h/6s. Villain (ROBYNBLUFF05, SB) raises
//     preflop to EUR0.20 (native 200); hero calls.
//     Flop 4d 5s 8s: hero (OOP, acts first) checks, villain bets EUR0.15
//     (native 150), hero calls.
//     Turn 3d: hero checks, villain checks.
//     River Js: hero bets EUR0.30 (native 300) -- an OPENING action (hero
//     is OOP/BB and acts FIRST on every postflop street here, unlike hand 1
//     where hero was IP/SB and acted last) -- villain calls.
//   Real log (server.log): "[DH_STRATEGY] RIVER hand=6h6s pot=700 expl=0.94%:
//   call=0.14% raise(0.50x pot)=99.09% raise(1.00x pot)=0.64%
//   raise(2.00x pot)=0.12% allin=0.01%" -- a full 5-branch ladder (unlike
//   hand 1's 3-branch node), matching the real EUR0.30-on-EUR0.70-pot bet
//   (~43% pot, rounding to the "0.5x pot" abstraction bucket).
//
//   IMPORTANT DIFFERENCES FROM HAND 1's HARNESS:
//   - Hero is BB = slot 1 here (restart_game(1, ...)), not SB = slot 0.
//   - Hero acts FIRST every postflop street (OOP), so there is no villain
//     action to process before hero's own river decision -- the replay
//     stops right after Next_stage(3, river) + board-pruning, with no
//     opp_take_action() call for the river (villain hasn't acted yet at
//     the point hero must decide).
//   - Hero faces TWO actions on the flop (checks first, then calls
//     villain's bet) -- both narrowed via the same hero_range_local
//     postflop mechanism, matching each real observed action in order.
//   - FIX vs hand 1's harness: narrow_villain_range_postflop() (real
//     production code, dh_native_ai.cpp line ~1065) ALWAYS hardcodes
//     extended_actions=true for EVERY observed byte ('d','l',2,'n' alike)
//     since that resolver is only ever used to compute a narrowing update,
//     never to pick hero's own action. Hand 1's harness inconsistently
//     passed false for hero's plain-check narrowing calls; this harness
//     uses true uniformly for hero's postflop narrowing too, for a more
//     faithful mirror of villain's exact, documented convention.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_hand_12473147059_texassolver_compare tools/test_hand_12473147059_texassolver_compare.cpp
//   RUN (from PokerAI/):
//     DH_VERBOSE_STRATEGY=1 ./tools/test_hand_12473147059_texassolver_compare
//###############################################################################
#include "dh_native_ai.cpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>

static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}

static std::vector<WeightedHand> hero_range_local;

static void init_hero_range_local() {
	hero_range_local.clear();
	for (int c1 = 0; c1 < 52; c1++)
		for (int c2 = c1 + 1; c2 < 52; c2++)
			hero_range_local.push_back({ (unsigned char)c1, (unsigned char)c2, 0.0 });
	double w = 1.0 / (double)hero_range_local.size();
	for (auto& h : hero_range_local) h.weight = w;
}

static void prune_hero_range_local_for_board() {
	std::vector<WeightedHand> kept;
	kept.reserve(hero_range_local.size());
	double sum = 0.0;
	for (auto& h : hero_range_local) {
		bool collide = false;
		for (unsigned char b : g.board) if (h.c1 == b || h.c2 == b) { collide = true; break; }
		if (collide) continue;
		kept.push_back(h);
		sum += h.weight;
	}
	if (sum > 1e-12) for (auto& h : kept) h.weight /= sum;
	hero_range_local = std::move(kept);
}

static void narrow_hero_range_local_preflop(unsigned char observed_byte) {
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
		for (auto& h : hero_range_local) {
			unsigned char hand[2] = { h.c1, h.c2 };
			int cluster = engine->get_preflop_cluster(hand);
			double p = (cluster >= 0 && cluster < (int)res.probs.size()) ? res.probs[cluster][idx] : 0.0;
			h.weight *= p;
			sum += h.weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("hero range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : hero_range_local) h.weight /= sum;
		std::printf("[HERO_RANGE_LOCAL] preflop narrowed using byte=%d (%zu combos)\n",
			(int)observed_byte, hero_range_local.size());
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[HERO_RANGE_LOCAL] preflop hero-range narrowing failed (%s) -- range left unchanged\n", e.what());
	}
}

static void narrow_hero_range_local_postflop(unsigned char observed_byte) {
	if (hero_range_local.empty() || g.villain_range.empty()) return;
	try {
		Searchstate s = build_current_searchstate(g.my_id);
		std::vector<std::array<unsigned char, 2>> hero_hands, villain_hands;
		hero_hands.reserve(hero_range_local.size());
		for (auto& h : hero_range_local) hero_hands.push_back({ h.c1, h.c2 });
		villain_hands.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) villain_hands.push_back({ h.c1, h.c2 });

		// LiveResolver/Players_range address the two seats by ABSOLUTE
		// SLOT (range.hero == slot 0, range.villain == slot 1 -- see
		// dh_native_ai.cpp's build_resolver_ranges()/range_for_slot(),
		// which every production caller uses and which this local
		// reimplementation must mirror), NOT by "our own bot" identity.
		// This hand has hero in slot 1 (BB/OOP): assigning "our own bot"
		// straight to range.hero (as if hero were always slot 0, true
		// only for hand 1's harness where hero WAS slot 0) silently
		// swaps N/M and crashes inside average_strategy() below with an
		// out-of-bounds strat_sum access once the resolver's internal
		// own_n stops matching hero_range_local's real size. Route through
		// g.my_id exactly like range_for_slot() does so this works for
		// either seat.
		bool hero_is_slot0 = (g.my_id == 0);
		Players_range range;
		range.hero = hero_is_slot0 ? hero_hands : villain_hands;
		range.villain = hero_is_slot0 ? villain_hands : hero_hands;

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
		// extended_actions=true unconditionally, mirroring
		// narrow_villain_range_postflop()'s own hardcoded choice (dh_native_ai.cpp
		// ~line 1065): this resolver only ever computes a narrowing update, never
		// picks hero's real action, so the extra 4th branch is always safe/used.
		LiveResolver resolver(range, engine, leaf.get(), mode, /*extended_actions=*/true, river_leaf.get());
		resolver.init_root(s, g.board);

		std::vector<double> hero_weights, villain_weights;
		hero_weights.reserve(hero_range_local.size());
		for (auto& h : hero_range_local) hero_weights.push_back(h.weight);
		villain_weights.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) villain_weights.push_back(h.weight);
		// Same slot-based routing as range.hero/range.villain above --
		// external_reach0 must line up with whichever seat is slot 0.
		const std::vector<double>* reach0 = hero_is_slot0 ? &hero_weights : &villain_weights;
		const std::vector<double>* reach1 = hero_is_slot0 ? &villain_weights : &hero_weights;

		run_until_converged(resolver, mode, reach0, reach1);

		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

		// hero_range_local occupies the resolver's slot-0 index space
		// when hero is slot 0, slot-1 index space otherwise -- but
		// average_strategy() only needs a valid index into whichever
		// space hero's combo actually lives in (N if hero_is_slot0, M
		// otherwise), and since range.hero/range.villain were assigned
		// above so that hero_hands occupies exactly the space matching
		// hero_range_local's own index order, `i` here already lines up
		// 1:1 with hero_range_local -- no separate remap needed.
		double sum = 0.0;
		for (size_t i = 0; i < hero_range_local.size(); i++) {
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
			hero_range_local[i].weight *= avg[idx];
			sum += hero_range_local[i].weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("hero range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : hero_range_local) h.weight /= sum;
		std::printf("[HERO_RANGE_LOCAL] postflop narrowed using byte=%d (%zu combos)\n",
			(int)observed_byte, hero_range_local.size());
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[HERO_RANGE_LOCAL] postflop hero-range narrowing failed (%s) -- range left unchanged\n", e.what());
	}
}

static void dump_range_texassolver(const std::vector<WeightedHand>& range, const char* path) {
	std::ofstream f(path);
	bool first = true;
	for (auto& h : range) {
		if (!(h.weight > 0.0)) continue;
		if (!first) f << ",";
		first = false;
		f << dh_card_str(h.c1) << dh_card_str(h.c2) << ":" << h.weight;
	}
	f.close();
}

int main() {
	int hero_c1 = card_id("6h"), hero_c2 = card_id("6s");

	restart_game(1, hero_c1, hero_c2); // hero = nosami = BB = slot 1, matching the real hand
	init_hero_range_local();

	// Preflop: villain (SB, slot 0) opens to 200 (EUR0.20); hero (BB) calls.
	opp_take_action((char*)"raise 200"); // villain's opening raise (auto-narrows g.villain_range + pushes path)
	narrow_hero_range_local_preflop((unsigned char)'l'); // hero's plain call
	apply_own_action("call");
	g.preflop_action_path.push_back((unsigned char)'l');

	unsigned char flop[3] = { (unsigned char)card_id("4d"), (unsigned char)card_id("5s"), (unsigned char)card_id("8s") };
	Next_stage(1, (char*)flop);
	prune_hero_range_local_for_board();

	// Hero (OOP) checks first on the flop.
	narrow_hero_range_local_postflop((unsigned char)'l');
	apply_own_action("call");
	// Villain bets 150 (EUR0.15, street-relative opening bet this street).
	opp_take_action((char*)"raise 150");
	// Hero calls villain's flop bet.
	narrow_hero_range_local_postflop((unsigned char)'l');
	apply_own_action("call");

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("3d") };
	Next_stage(2, (char*)turn);
	prune_hero_range_local_for_board();

	// Hero (OOP) checks first on the turn; villain checks back.
	narrow_hero_range_local_postflop((unsigned char)'l');
	apply_own_action("call");
	opp_take_action((char*)"call");

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("Js") };
	Next_stage(3, (char*)river);
	prune_hero_range_local_for_board();

	// Hero is first to act on the river (OOP) -- no villain action precedes
	// this decision, so no opp_take_action() call here (unlike hand 1).
	g.hero_range = hero_range_local;

	int pot = (20000 - g.stack[0]) + (20000 - g.stack[1]);
	int eff_stack = std::min(g.stack[0], g.stack[1]);
	std::printf("\n=== INTERNAL STATE AT RIVER DECISION (fixed 20000-stack convention) ===\n");
	std::printf("pot=%d eff_stack=%d hero_combos=%zu villain_combos=%zu\n",
		pot, eff_stack, g.hero_range.size(), g.villain_range.size());

	dump_range_texassolver(g.hero_range, "/tmp/hand2_hero_range_texassolver.txt");
	dump_range_texassolver(g.villain_range, "/tmp/hand2_villain_range_texassolver.txt");
	std::printf("Dumped hero range -> /tmp/hand2_hero_range_texassolver.txt\n");
	std::printf("Dumped villain range -> /tmp/hand2_villain_range_texassolver.txt\n");

	// Validation against the real log: "hand=6h6s pot=700 expl=0.94%:
	// call=0.14% raise(0.50x pot)=99.09% raise(1.00x pot)=0.64%
	// raise(2.00x pot)=0.12% allin=0.01%" (DH_VERBOSE_STRATEGY=1 required).
	std::string action = resolve_decision();
	std::printf("resolve_decision() returned: %s\n", action.c_str());

	return 0;
}
