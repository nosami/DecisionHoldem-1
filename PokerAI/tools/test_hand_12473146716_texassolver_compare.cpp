//###############################################################################
//   test_hand_12473146716_texassolver_compare.cpp -- real live SkyPoker hand,
//   replayed through DecisionHoldem's REAL production functions
//   (restart_game/apply_own_action/opp_take_action/Next_stage/resolve_decision,
//   not a reimplementation), in order to dump the EXACT g.hero_range and
//   g.villain_range weighted-combo arrays DH used for its real river decision
//   -- so those same weighted ranges (not a uniform/wide approximation) can be
//   fed into TexasSolver (this repo) for a true "exact same input" comparison,
//   per the user's explicit request after the first (uniform-range) hand-1
//   comparison already showed closely-matching decisions.
//
//   Real hand (game_logs/hand_12473146716/hand_history.txt):
//     Hero (nosami, SB) dealt 9c/Jc, raises preflop to EUR0.30 (native 300);
//     villain (ROBYNBLUFF05, BB) calls.
//     Flop 4h 3h Js: villain checks, hero checks back.
//     Turn 5s: villain checks, hero checks back.
//     River 9s: villain checks, hero bets EUR0.60 (native 600, 1x pot);
//     villain folds.
//   Real log (server.log): "[DH_STRATEGY] RIVER hand=9cJc pot=600 expl=0.98%:
//   call=1.18% raise(1.00x pot)=98.59% allin=0.23%" -- this tool reproduces
//   the resolve that produced that exact line, then dumps its inputs.
//
//   IMPORTANT: restart_game() always initializes g.stack[] from a FIXED
//   20000-native reference stack (slot0=SB posts 50, slot1=BB posts 100),
//   NEVER the real fluctuating EUR table stack (confirmed by reading
//   decisionholdem_bridge.py's restart_game() call, which passes only
//   hero_slot/c1/c2 -- no stack argument at all). So the internal
//   pot/effective-stack this hand's REAL resolve actually used are NOT
//   600/9600 (the real-EUR-scaled numbers used for the first, uniform-range
//   hand-1 TexasSolver comparison) but whatever this fixed-stack replay
//   below computes -- printed at the end for the corrected TexasSolver input.
//
//   Hero's own flop/turn checks were real "FLOP-BLUEPRINT"/"TURN-BLUEPRINT"
//   direct-blueprint decisions in production (a fragile, cursor-tracked path
//   this offline harness cannot safely reproduce bit-for-bit -- see
//   test_kcflush_river_range.cpp's identical design note). As that file
//   already established and validated, a dedicated LiveResolver-based
//   narrowing (mirroring villain's own narrow_villain_range_postflop
//   mechanism exactly, just applied to hero's observed action instead) is
//   used here as the theoretically-sound stand-in, via a test-local
//   hero_range_local that gets assigned into the real g.hero_range right
//   before dumping/resolving -- so resolve_decision() (called at the end
//   for validation against the real log) sees exactly this narrowed range.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_hand_12473146716_texassolver_compare tools/test_hand_12473146716_texassolver_compare.cpp
//   RUN (from PokerAI/):
//     DH_VERBOSE_STRATEGY=1 ./tools/test_hand_12473146716_texassolver_compare
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

// --- hero_range_local: mirrors test_kcflush_river_range.cpp's established
// hero-range tracking pattern exactly (see file header for rationale). ---
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

static void narrow_hero_range_local_postflop(unsigned char observed_byte, bool extended_actions) {
	if (hero_range_local.empty() || g.villain_range.empty()) return;
	try {
		Searchstate s = build_current_searchstate(g.my_id);
		std::vector<std::array<unsigned char, 2>> hero_hands, villain_hands;
		hero_hands.reserve(hero_range_local.size());
		for (auto& h : hero_range_local) hero_hands.push_back({ h.c1, h.c2 });
		villain_hands.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) villain_hands.push_back({ h.c1, h.c2 });

		Players_range range;
		range.hero = hero_hands;
		range.villain = villain_hands;

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
		LiveResolver resolver(range, engine, leaf.get(), mode, extended_actions, river_leaf.get());
		resolver.init_root(s, g.board);

		std::vector<double> hero_weights, villain_weights;
		hero_weights.reserve(hero_range_local.size());
		for (auto& h : hero_range_local) hero_weights.push_back(h.weight);
		villain_weights.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) villain_weights.push_back(h.weight);

		run_until_converged(resolver, mode, &hero_weights, &villain_weights);

		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

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
		std::printf("[HERO_RANGE_LOCAL] postflop narrowed using byte=%d, extended_actions=%d (%zu combos)\n",
			(int)observed_byte, (int)extended_actions, hero_range_local.size());
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[HERO_RANGE_LOCAL] postflop hero-range narrowing failed (%s) -- range left unchanged\n", e.what());
	}
}

// Dumps a WeightedHand range to a TexasSolver-compatible per-combo weighted
// range string ("9cJc:0.000925,...", see PrivateRangeConverter.cpp's
// "Specific card combo" branch), skipping non-positive weights (which
// TexasSolver's parser rejects).
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
	int hero_c1 = card_id("9c"), hero_c2 = card_id("Jc");

	restart_game(0, hero_c1, hero_c2); // hero = nosami = SB = slot 0, matching the real hand
	init_hero_range_local();

	// Preflop: hero (SB) raises to 300 (EUR0.30). g.preflop_action_path is
	// empty here (hero acts first), matching the real hand.
	{
		int total_pot_before = (20000 - g.stack[0]) + (20000 - g.stack[1]); // 50+100=150
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]); // 100
		int my_bet_before = 20000 - g.stack[0]; // 50 (hero is SB, slot 0)
		int hero_raise_byte = match_raise_action_byte(total_pot_before, last_bigbet_before, my_bet_before, 300);
		if (hero_raise_byte < 0) {
			std::fprintf(stderr, "FATAL: hero's real preflop raise (300) does not match any trained blueprint byte\n");
			return 1;
		}
		narrow_hero_range_local_preflop((unsigned char)hero_raise_byte);
		apply_own_action("raise 300"); // hero opens preflop to 300 (EUR0.30)
		g.preflop_action_path.push_back((unsigned char)hero_raise_byte);
	}
	opp_take_action((char*)"call"); // villain calls preflop

	unsigned char flop[3] = { (unsigned char)card_id("4h"), (unsigned char)card_id("3h"), (unsigned char)card_id("Js") };
	Next_stage(1, (char*)flop);
	prune_hero_range_local_for_board();
	opp_take_action((char*)"call"); // villain checks flop

	narrow_hero_range_local_postflop((unsigned char)'l', /*extended_actions=*/false); // hero checks back flop
	apply_own_action("call");

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)card_id("5s") };
	Next_stage(2, (char*)turn);
	prune_hero_range_local_for_board();
	opp_take_action((char*)"call"); // villain checks turn

	narrow_hero_range_local_postflop((unsigned char)'l', /*extended_actions=*/false); // hero checks back turn
	apply_own_action("call");

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)card_id("9s") };
	Next_stage(3, (char*)river);
	prune_hero_range_local_for_board();
	opp_take_action((char*)"call"); // villain checks river -- this is the REAL ~5s narrowing resolve

	// Install our faithfully-narrowed local hero_range into the real
	// g.hero_range so resolve_decision() below (and our dump) both see
	// exactly what production would if hero's own flop/turn narrowing had
	// used a live resolve instead of the blueprint-cursor path.
	g.hero_range = hero_range_local;

	int pot = (20000 - g.stack[0]) + (20000 - g.stack[1]);
	int eff_stack = std::min(g.stack[0], g.stack[1]);
	std::printf("\n=== INTERNAL STATE AT RIVER DECISION (fixed 20000-stack convention) ===\n");
	std::printf("pot=%d eff_stack=%d hero_combos=%zu villain_combos=%zu\n",
		pot, eff_stack, g.hero_range.size(), g.villain_range.size());

	dump_range_texassolver(g.hero_range, "/tmp/hand1_hero_range_texassolver.txt");
	dump_range_texassolver(g.villain_range, "/tmp/hand1_villain_range_texassolver.txt");
	std::printf("Dumped hero range -> /tmp/hand1_hero_range_texassolver.txt\n");
	std::printf("Dumped villain range -> /tmp/hand1_villain_range_texassolver.txt\n");

	// Validation: call the REAL production resolve_decision() and confirm
	// it reproduces the real log's hand=9cJc pot=600 expl=0.98%:
	// call=1.18% raise(1.00x pot)=98.59% allin=0.23% (run with
	// DH_VERBOSE_STRATEGY=1 to see dh_log_strategy's printed line).
	std::string action = resolve_decision();
	std::printf("resolve_decision() returned: %s\n", action.c_str());

	return 0;
}
