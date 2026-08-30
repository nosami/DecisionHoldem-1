//###############################################################################
//   test_kcflush_river_range.cpp -- user-flagged live SkyPoker hand
//   12472520778: hero (nosami, SB) held 2c/Qc, made a KING-HIGH CLUB FLUSH
//   on the river (board Jc 5s 3s 5c Kc: hero's Kc/Jc/5c board clubs plus
//   Qc/2c in hand = flush K-Q-J-5-2 -- second nuts, beaten only by an
//   ace-high club flush or quad fives), and the live engine folded this
//   hand river=fold=99.65%/96.28%/99.68% (varies run to run -- see the real
//   captured server.log: "RIVER hand=2cQc pot=4000 expl=0.25%: fold=96.28%
//   call=3.62% raise(1.00x pot)=0.05% allin=0.05%"). The user firmly
//   rejected an initial "not a bug" characterization, correctly pointing
//   out (a) this was second nuts, not just "a flush", and (b) the decision
//   ran through the REAL-TIME RIVER RESOLVE (dh_log_strategy's plain
//   "RIVER" label with a genuine expl=X%), not a static blueprint lookup
//   (which would be labelled "RIVER-BLUEPRINT" with expl=n/a, as flop/turn
//   were in this same hand) -- so the fold's correctness depends entirely
//   on the quality of the DH_RANGE_MODEL opponent-range narrowing fed into
//   that resolve, which BUILD_NOTES.md section 47 already documented as a
//   known source of both genuine misses (Hand 1: a real underweighted
//   holding) AND correctly-folded hands that only look surprising until
//   the model's own top combos are examined (Hand 2).
//
//   This tool replays hand 12472520778's exact real sequence through the
//   real production functions (dh_native_ai.cpp's own
//   opp_take_action()/apply_own_action()/Next_stage(), not a
//   reimplementation), then -- right after villain's real river bet has
//   been narrowed into g.villain_range, at the exact point the live engine
//   would call getdecision() -- classifies EVERY one of the ~990 tracked
//   villain combos against hero's fixed 7-card board+hole using a
//   self-contained best-of-7 evaluator (no dependency on the multi-GB
//   Engine/cluster files: this board's category set is simple enough --
//   no straight is reachable at all, see the analysis below -- to verify
//   by hand AND cross-check programmatically), and sums the weighted mass
//   split between "beats hero" and "loses to hero".
//
//   Real hand sequence (game_logs/hand_12472520778/server.log):
//     Hero (SB, 2c/Qc) opens preflop to 200 (native chips; EUR0.20 real at
//     this hand's EUR0.10 big blind), villain (BB) calls.
//     Flop Jc 5s 3s: villain checks, hero bets 800 (EUR0.80), villain
//     calls.
//     Turn 5c (board pairs fives): villain checks, hero checks back.
//     River Kc (hero's flush completes): villain bets 2000 -- a pot-sized
//     bet (pot was exactly 2000 native going to the river) -- hero folds.
//
//   Manual case analysis of every one of the 990 non-blocked villain
//   combos on this exact board (Jc 5s 3s 5c Kc), before running this
//   tool: no straight is reachable (board ranks {3,5,J,K} are too
//   scattered for 2 hole cards to bridge into 5 consecutive ranks), so
//   the only categories above a flush are full house and quads, and both
//   require pairing the board's OWN paired rank (fives) since that is the
//   only rank with 2 copies already on board:
//     - Quad fives: 5d5h (1 combo) -- beats hero.
//     - Full house, fives full of kings: {5d,5h} x {Kd,Kh,Ks} (6 combos)
//       -- beats hero.
//     - Full house, fives full of jacks: {5d,5h} x {Jd,Jh,Js} (6 combos)
//       -- beats hero.
//     - Full house, fives full of threes: {5d,5h} x {3c,3d,3h} (6 combos)
//       -- beats hero.
//     - Ace-high club flush: Ac + one of {3c,4c,6c,7c,8c,9c,Tc} (7 combos)
//       -- beats hero outright (Ace beats King as the flush's top card,
//       regardless of the second card -- Qc/Jc are blocked so no non-Ace
//       club combo can out-rank hero's Q kicker).
//   Total combos that beat hero: 1 + 6 + 6 + 6 + 7 = 26 (out of 990).
//   Every other combo (two pair, one pair, trips of jacks/kings/threes
//   via a pocket pair, high card, or a non-ace flush) is categorically
//   below hero's flush or loses the flush-vs-flush comparison, so loses.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_kcflush_river_range tools/test_kcflush_river_range.cpp
//   RUN (from PokerAI/):
//     DH_VERBOSE_STRATEGY=1 ./tools/test_kcflush_river_range
//###############################################################################
#include "dh_native_ai.cpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

// This file's card-id convention (see dh_card_str()'s own comment):
// id = suit*13 + rank, suits "scdh" (s=0,c=1,d=2,h=3), ranks "23456789TJQKA".
static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}
static int card_rank(int id) { return id % 13; } // 0='2' .. 12='A'
static int card_suit(int id) { return id / 13; } // 0=s 1=c 2=d 3=h

enum Category {
	HIGH_CARD = 0, PAIR = 1, TWO_PAIR = 2, TRIPS = 3, STRAIGHT = 4,
	FLUSH = 5, FULL_HOUSE = 6, QUADS = 7, STRAIGHT_FLUSH = 8
};

struct HandScore {
	int cat = -1;
	int tie[5] = { -1,-1,-1,-1,-1 };
	bool operator<(const HandScore& o) const {
		if (cat != o.cat) return cat < o.cat;
		for (int i = 0; i < 5; i++) if (tie[i] != o.tie[i]) return tie[i] < o.tie[i];
		return false;
	}
};

// Best-of-7 evaluator: standard rank/suit-counting shortcuts (no need to
// enumerate all C(7,5)=21 subsets -- a flush, when present among 7 cards,
// always has exactly the top-5-ranked cards of the majority suit as its
// best 5, and a straight is a straight regardless of which "extra" cards
// are unused).
static HandScore evaluate7(const std::vector<int>& ids) {
	int rankcnt[13] = { 0 };
	int suitcnt[4] = { 0 };
	for (int id : ids) { rankcnt[card_rank(id)]++; suitcnt[card_suit(id)]++; }

	int flush_suit = -1;
	for (int s = 0; s < 4; s++) if (suitcnt[s] >= 5) flush_suit = s;

	bool present[13] = { false };
	for (int id : ids) present[card_rank(id)] = true;
	auto find_straight_high = [](const bool* p) {
		int high = -1;
		if (p[12] && p[0] && p[1] && p[2] && p[3]) high = 3; // wheel: A-2-3-4-5
		for (int hi = 4; hi <= 12; hi++)
			if (p[hi] && p[hi-1] && p[hi-2] && p[hi-3] && p[hi-4]) high = hi;
		return high;
	};
	int straight_high = find_straight_high(present);

	if (flush_suit >= 0) {
		bool fpresent[13] = { false };
		for (int id : ids) if (card_suit(id) == flush_suit) fpresent[card_rank(id)] = true;
		int sf_high = find_straight_high(fpresent);
		if (sf_high >= 0) return HandScore{ STRAIGHT_FLUSH, { sf_high,-1,-1,-1,-1 } };
	}

	std::vector<std::pair<int,int>> groups; // (count, rank), rank desc as tiebreak
	for (int r = 12; r >= 0; r--) if (rankcnt[r] > 0) groups.push_back({ rankcnt[r], r });
	std::stable_sort(groups.begin(), groups.end(), [](auto& a, auto& b) { return a.first > b.first; });

	if (groups[0].first == 4) {
		int kicker = -1;
		for (auto& g : groups) if (g.second != groups[0].second) { kicker = g.second; break; }
		return HandScore{ QUADS, { groups[0].second, kicker, -1,-1,-1 } };
	}
	if (groups[0].first == 3 && groups.size() > 1 && groups[1].first >= 2)
		return HandScore{ FULL_HOUSE, { groups[0].second, groups[1].second, -1,-1,-1 } };
	if (flush_suit >= 0) {
		std::vector<int> ranks;
		for (int id : ids) if (card_suit(id) == flush_suit) ranks.push_back(card_rank(id));
		std::sort(ranks.rbegin(), ranks.rend());
		HandScore s{ FLUSH, { -1,-1,-1,-1,-1 } };
		for (int i = 0; i < 5 && i < (int)ranks.size(); i++) s.tie[i] = ranks[i];
		return s;
	}
	if (straight_high >= 0) return HandScore{ STRAIGHT, { straight_high,-1,-1,-1,-1 } };
	if (groups[0].first == 3) {
		int k1 = -1, k2 = -1;
		for (size_t i = 1; i < groups.size(); i++) {
			if (k1 < 0) k1 = groups[i].second;
			else if (k2 < 0) { k2 = groups[i].second; break; }
		}
		return HandScore{ TRIPS, { groups[0].second, k1, k2, -1,-1 } };
	}
	if (groups[0].first == 2 && groups.size() > 1 && groups[1].first == 2) {
		int kicker = groups.size() > 2 ? groups[2].second : -1;
		return HandScore{ TWO_PAIR, { groups[0].second, groups[1].second, kicker, -1,-1 } };
	}
	if (groups[0].first == 2) {
		int k[3] = { -1,-1,-1 }; int ki = 0;
		for (size_t i = 1; i < groups.size() && ki < 3; i++) k[ki++] = groups[i].second;
		return HandScore{ PAIR, { groups[0].second, k[0], k[1], k[2], -1 } };
	}
	int k[5] = { -1,-1,-1,-1,-1 };
	for (size_t i = 0; i < groups.size() && i < 5; i++) k[i] = groups[i].second;
	return HandScore{ HIGH_CARD, { k[0], k[1], k[2], k[3], k[4] } };
}

// ---------------------------------------------------------------------------
// Hero-range tracking (test-tool-only). The user correctly rejected pinning
// hero to its one true combo (2c/Qc) when cross-checking this hand: (a) it
// throws away any genuine bluffing/balance consideration on hero's side, and
// (b) it lets the OTHER side's (villain's) strategy at the root become
// degenerately exploitative, since villain's node gets solved as if villain
// had certain knowledge hero holds exactly one combo -- not a realistic GTO
// setup. The user's own proposed fix -- "hero's range can be calculated the
// exact same way as villain's range" -- is implemented here: hero_range is a
// SEPARATE persistent, full (no fixed pool size) belief tracked across the
// whole hand, narrowed after each of HERO's OWN real actions using the same
// production mechanisms dh_native_ai.cpp already uses for villain
// (preflop: the real trained blueprint's per-cluster action probabilities;
// postflop: a dedicated LiveResolver run), rather than being pinned.
//
// One real methodological asymmetry remains, and is deliberate, not an
// oversight: postflop narrowing here runs a LiveResolver with BOTH sides as
// genuine multi-combo ranges (hero_range AND villain's CONTEMPORANEOUS
// tracked g.villain_range, as it stands at that exact point in the
// sequential replay -- never a look-ahead to villain's more-fully-narrowed
// future range), so neither side is pinned to a single hand. This differs
// from hero's REAL flop/turn decisions in this hand, which were direct
// blueprint-table lookups (see the "FLOP-BLUEPRINT"/"TURN-BLUEPRINT" log
// labels) rather than live resolves -- reproducing that exact mechanism for
// hero's own actions would require making this offline replay harness also
// correctly advance g.blueprint_node/g.preflop_action_path for hero's own
// moves (apply_own_action() below deliberately does not do this today; only
// resolve_preflop_decision()/resolve_direct_blueprint_decision(), the real
// production decision functions this harness does NOT call, do). A live
// resolve is used instead as a theoretically sound, mutually-consistent
// stand-in that avoids that extra, fragile cursor-tracking risk.
static std::vector<WeightedHand> hero_range;

static void init_hero_range() {
	hero_range.clear();
	for (int c1 = 0; c1 < 52; c1++)
		for (int c2 = c1 + 1; c2 < 52; c2++)
			hero_range.push_back({ (unsigned char)c1, (unsigned char)c2, 0.0 });
	double w = 1.0 / (double)hero_range.size();
	for (auto& h : hero_range) h.weight = w;
}

// Mirrors prune_villain_range_for_board() (dh_native_ai.cpp) exactly,
// applied to hero_range. Must be called after every Next_stage() -- unlike
// villain_range, this isn't automatic, since Next_stage() only knows about
// LiveGame's own villain_range field.
static void prune_hero_range_for_board() {
	std::vector<WeightedHand> kept;
	kept.reserve(hero_range.size());
	double sum = 0.0;
	for (auto& h : hero_range) {
		bool collide = false;
		for (unsigned char b : g.board) if (h.c1 == b || h.c2 == b) { collide = true; break; }
		if (collide) continue;
		kept.push_back(h);
		sum += h.weight;
	}
	if (sum > 1e-12) for (auto& h : kept) h.weight /= sum;
	hero_range = std::move(kept);
}

// Mirrors narrow_villain_range_preflop() exactly (dh_native_ai.cpp), applied
// to hero_range. Must be called with g.preflop_action_path AS IT STOOD
// BEFORE hero's own action is appended to it (mirroring
// resolve_preflop_decision()'s real "look up, then append" order).
static void narrow_hero_range_preflop(unsigned char observed_byte) {
	if (!g.preflop_path_confident) return;
	try {
		BlueprintReader::AllClustersResult res;
		bool used_cache = false;
		if (g_preflop_cache_loaded) {
			try {
				res = PreflopCache::lookup_preflop_strategy_all_clusters(g_preflop_cache, g.preflop_action_path);
				used_cache = true;
			} catch (const std::exception&) {
				// Cache miss/failure for this specific path -- fall through
				// to the disk walk below, exactly as narrow_villain_range_preflop() does.
			}
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
		for (auto& h : hero_range) {
			unsigned char hand[2] = { h.c1, h.c2 };
			int cluster = engine->get_preflop_cluster(hand);
			double p = (cluster >= 0 && cluster < (int)res.probs.size()) ? res.probs[cluster][idx] : 0.0;
			h.weight *= p;
			sum += h.weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("hero range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : hero_range) h.weight /= sum;
		std::printf("[HERO_RANGE_MODEL] preflop narrowed hero_range using byte=%d (%zu combos)\n",
			(int)observed_byte, hero_range.size());
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[HERO_RANGE_MODEL] preflop hero-range narrowing failed (%s) -- "
			"range left unchanged for this action\n", e.what());
	}
}

// Mirrors narrow_villain_range_postflop() (dh_native_ai.cpp), but -- unlike
// that function, which always pins ONE side to hero's single true real hand
// -- runs the LiveResolver with BOTH sides as genuine multi-combo ranges:
// hero_range (narrowed so far) and villain's CONTEMPORANEOUS g.villain_range
// snapshot, passed as external_reach0/external_reach1 together. This is the
// "calculate hero's range the exact same way as villain's" fix the user
// asked for. Must be called BEFORE apply_own_action() mutates g.stack for
// this action (mirrors every other narrowing call's ordering requirement).
static void narrow_hero_range_postflop(unsigned char observed_byte, bool extended_actions) {
	if (hero_range.empty() || g.villain_range.empty()) return;
	try {
		Searchstate s = build_current_searchstate(g.my_id);
		std::vector<std::array<unsigned char, 2>> hero_hands, villain_hands;
		hero_hands.reserve(hero_range.size());
		for (auto& h : hero_range) hero_hands.push_back({ h.c1, h.c2 });
		villain_hands.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) villain_hands.push_back({ h.c1, h.c2 });

		Players_range range;
		// Hero occupies seat/slot g.my_id == 0 in this hand (see
		// restart_game(0, ...) in main()), so range.hero (=seat 0's
		// container, per Players_range's own field comment) holds hero's
		// own candidates and range.villain (=seat 1's) holds villain's.
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
		hero_weights.reserve(hero_range.size());
		for (auto& h : hero_range) hero_weights.push_back(h.weight);
		villain_weights.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) villain_weights.push_back(h.weight);

		run_until_converged(resolver, mode, &hero_weights, &villain_weights);

		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

		double sum = 0.0;
		for (size_t i = 0; i < hero_range.size(); i++) {
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
			hero_range[i].weight *= avg[idx];
			sum += hero_range[i].weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("hero range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : hero_range) h.weight /= sum;
		std::printf("[HERO_RANGE_MODEL] postflop narrowed hero_range using byte=%d, extended_actions=%d (%zu combos)\n",
			(int)observed_byte, (int)extended_actions, hero_range.size());
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[HERO_RANGE_MODEL] postflop hero-range narrowing failed (%s) -- "
			"range left unchanged for this action\n", e.what());
	}
}

static const char* category_name(int cat) {
	switch (cat) {
		case HIGH_CARD: return "high card";
		case PAIR: return "pair";
		case TWO_PAIR: return "two pair";
		case TRIPS: return "trips";
		case STRAIGHT: return "straight";
		case FLUSH: return "flush";
		case FULL_HOUSE: return "full house";
		case QUADS: return "quads";
		case STRAIGHT_FLUSH: return "straight flush";
	}
	return "?";
}

int main() {
	int hero_c1 = card_id("2c"), hero_c2 = card_id("Qc");
	int board[5] = {
		card_id("Jc"), card_id("5s"), card_id("3s"), card_id("5c"), card_id("Kc")
	};

	// Sanity-check the evaluator against hero's own known hand before
	// trusting it for the full range: hero's 7 cards must evaluate to a
	// FLUSH with tiebreak K,Q,J,5,2 (ranks 11,10,9,3,0).
	{
		std::vector<int> hero7 = { hero_c1, hero_c2, board[0], board[1], board[2], board[3], board[4] };
		HandScore hs = evaluate7(hero7);
		assert(hs.cat == FLUSH);
		assert(hs.tie[0] == 11 && hs.tie[1] == 10 && hs.tie[2] == 9 && hs.tie[3] == 3 && hs.tie[4] == 0);
		std::printf("[SELF-TEST] hero7 = %s (cat=%s tie=%d,%d,%d,%d,%d) -- OK\n\n",
			"2c Qc / Jc 5s 3s 5c Kc", category_name(hs.cat),
			hs.tie[0], hs.tie[1], hs.tie[2], hs.tie[3], hs.tie[4]);
	}
	std::vector<int> hero7 = { hero_c1, hero_c2, board[0], board[1], board[2], board[3], board[4] };
	HandScore hero_score = evaluate7(hero7);

	restart_game(0, hero_c1, hero_c2); // hero is SB (slot 0), matching the real hand
	init_hero_range(); // NEW: hero's own tracked belief, narrowed the same way villain's is

	// Preflop: hero (SB) opens to 200. g.preflop_action_path is empty here
	// (hero acts first), so this narrowing call and the real byte-match
	// below both use the empty root path -- unambiguous, no ordering bug
	// possible for THIS specific action.
	{
		int total_pot_before = (20000 - g.stack[0]) + (20000 - g.stack[1]);
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
		int my_bet_before = 20000 - g.stack[0]; // hero is slot 0
		int hero_raise_byte = match_raise_action_byte(total_pot_before, last_bigbet_before, my_bet_before, 200);
		if (hero_raise_byte < 0) {
			std::fprintf(stderr, "FATAL: hero's real preflop raise (200) does not match any trained blueprint byte\n");
			return 1;
		}
		narrow_hero_range_preflop((unsigned char)hero_raise_byte);
		apply_own_action("raise 200"); // hero opens preflop to 200 (EUR0.20)
		// BUG FIX: the pre-existing version of this replay never appended
		// hero's own raise byte to g.preflop_action_path, so villain's very
		// next preflop action below was narrowed using the EMPTY root path
		// (as if villain, not hero, were first to act) instead of "facing
		// hero's raise" -- silently corrupting villain's preflop narrowing
		// for the rest of the hand. resolve_preflop_decision()'s real
		// production code (line ~1310) always does this append immediately
		// after its own lookup; apply_own_action() here is a simplified
		// test-harness stand-in that does not, so it must be done explicitly
		// at every one of hero's real preflop actions in this replay.
		g.preflop_action_path.push_back((unsigned char)hero_raise_byte);
	}
	opp_take_action((char*)"call"); // villain calls preflop (now correctly narrowed against "facing hero's raise")

	unsigned char flop[3] = { (unsigned char)board[0], (unsigned char)board[1], (unsigned char)board[2] };
	Next_stage(1, (char*)flop);
	prune_hero_range_for_board(); // NEW
	opp_take_action((char*)"call"); // villain checks flop

	// Hero bets flop (800 on a 400 pot). The reduced LiveResolver action set
	// used for narrowing (see narrow_villain_range_postflop()'s own comment)
	// has no node for hero's exact 2x-pot sizing -- byte 2 ("extended
	// actions") is the same single canonical non-all-in-raise bucket
	// villain's own bet-sized raises are narrowed against elsewhere in this
	// file, so it's used here too for methodological consistency.
	narrow_hero_range_postflop((unsigned char)2, /*extended_actions=*/true);
	apply_own_action("raise 800"); // hero bets flop (EUR0.80)
	opp_take_action((char*)"call"); // villain calls flop bet

	unsigned char turn[4] = { flop[0], flop[1], flop[2], (unsigned char)board[3] };
	Next_stage(2, (char*)turn);
	prune_hero_range_for_board(); // NEW
	opp_take_action((char*)"call"); // villain checks turn

	// Hero checks back turn -- a plain 'l' (call/check) byte, no raise involved.
	narrow_hero_range_postflop((unsigned char)'l', /*extended_actions=*/false);
	apply_own_action("call"); // hero checks back turn

	unsigned char river[5] = { turn[0], turn[1], turn[2], turn[3], (unsigned char)board[4] };
	Next_stage(3, (char*)river);
	prune_hero_range_for_board(); // NEW
	opp_take_action((char*)"raise 2000"); // villain bets river, 1x pot (EUR2.00)

	// At this exact point, getdecision() would call resolve_decision() --
	// the real-time river resolve -- seeded by g.villain_range as it now
	// stands. Classify EVERY tracked combo.
	double beats_weight = 0.0, loses_weight = 0.0, ties_weight = 0.0;
	int beats_count = 0, loses_count = 0, ties_count = 0;
	double total_weight = 0.0;
	std::vector<std::tuple<double, std::string, const char*>> beats_detail; // weight, combo, category

	for (auto& h : g.villain_range) {
		total_weight += h.weight;
		std::vector<int> full7 = { (int)h.c1, (int)h.c2, board[0], board[1], board[2], board[3], board[4] };
		HandScore vs = evaluate7(full7);
		if (vs.cat > hero_score.cat || (vs.cat == hero_score.cat && !(vs < hero_score) && (hero_score < vs))) {
			beats_weight += h.weight; beats_count++;
			beats_detail.push_back({ h.weight, dh_card_str(h.c1) + dh_card_str(h.c2), category_name(vs.cat) });
		} else if (vs.cat == hero_score.cat && !(vs < hero_score) && !(hero_score < vs)) {
			ties_weight += h.weight; ties_count++;
		} else {
			loses_weight += h.weight; loses_count++;
		}
	}

	std::printf("=== hand 12472520778 river decision point ===\n");
	std::printf("hero: 2c Qc, board: Jc 5s 3s 5c Kc -- hero's best hand: %s\n",
		category_name(hero_score.cat));
	std::printf("tracked villain combos: %zu, total weight (sanity, should be ~1.0): %.6f\n\n",
		g.villain_range.size(), total_weight);

	std::printf("BEATS hero:  %3d combos, weight = %.4f%%\n", beats_count, beats_weight * 100.0);
	std::printf("TIES  hero:  %3d combos, weight = %.4f%%\n", ties_count, ties_weight * 100.0);
	std::printf("LOSES hero:  %3d combos, weight = %.4f%%\n\n", loses_count, loses_weight * 100.0);

	std::sort(beats_detail.rbegin(), beats_detail.rend());
	std::printf("Every combo that beats hero, highest tracked weight first:\n");
	for (auto& [w, combo, cat] : beats_detail)
		std::printf("  %-6s weight=%.4f%%  (%s)\n", combo.c_str(), w * 100.0, cat);

	std::printf("\nTop-25 tracked combos overall (beats/loses labelled):\n");
	std::vector<size_t> idx(g.villain_range.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
		return g.villain_range[a].weight > g.villain_range[b].weight;
	});
	for (int k = 0; k < 25 && k < (int)idx.size(); k++) {
		auto& h = g.villain_range[idx[k]];
		std::vector<int> full7 = { (int)h.c1, (int)h.c2, board[0], board[1], board[2], board[3], board[4] };
		HandScore vs = evaluate7(full7);
		bool wins = vs.cat > hero_score.cat || (vs.cat == hero_score.cat && hero_score < vs);
		std::printf("  #%2d %s%s = %.4f%%  %-12s %s\n", k + 1,
			dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0,
			category_name(vs.cat), wins ? "<- BEATS hero" : "");
	}

	// Dump the full tracked range in TexasSolver's "RankSuitRankSuit:weight,..."
	// range-string syntax, so it can be fed directly into this repo's own
	// independent GTO solver (build/console_solver) via set_range_oop, as a
	// cross-check against DecisionHoldem's own river decision for this hand.
	{
		const char* out_path = "/tmp/villain_river_range_kcflush.txt";
		FILE* f = std::fopen(out_path, "w");
		if (f) {
			bool first = true;
			for (auto& h : g.villain_range) {
				if (!first) std::fputc(',', f);
				first = false;
				std::fprintf(f, "%s%s:%.8f", dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight);
			}
			std::fclose(f);
			std::printf("\n[DUMP] wrote %zu combos in TexasSolver range syntax to %s\n",
				g.villain_range.size(), out_path);
		}
	}

	// Hero's OWN range, narrowed the exact same way villain's was (real
	// preflop blueprint + LiveResolver postflop resolves against villain's
	// contemporaneous tracked range at each point), instead of pinned to
	// hero's single true combo. Sanity-check hero's real hand (2c/Qc) is
	// still present, then dump in the same TexasSolver range syntax.
	{
		double hero_true_weight = -1.0;
		double hero_total_weight = 0.0;
		for (auto& h : hero_range) {
			hero_total_weight += h.weight;
			if ((int)h.c1 == hero_c1 && (int)h.c2 == hero_c2) hero_true_weight = h.weight;
			if ((int)h.c1 == hero_c2 && (int)h.c2 == hero_c1) hero_true_weight = h.weight;
		}
		std::printf("\n=== hero's own narrowed range (%zu combos, total weight (sanity, should be ~1.0): %.6f) ===\n",
			hero_range.size(), hero_total_weight);
		if (hero_true_weight < 0.0)
			std::printf("[WARNING] hero's real combo 2cQc is MISSING from hero_range -- something pruned it incorrectly!\n");
		else
			std::printf("hero's real combo 2cQc tracked weight: %.6f%% (rank within hero_range: ", hero_true_weight * 100.0);
		{
			std::vector<size_t> hidx(hero_range.size());
			for (size_t i = 0; i < hidx.size(); i++) hidx[i] = i;
			std::sort(hidx.begin(), hidx.end(), [&](size_t a, size_t b) {
				return hero_range[a].weight > hero_range[b].weight;
			});
			if (hero_true_weight >= 0.0) {
				for (size_t rank = 0; rank < hidx.size(); rank++) {
					auto& h = hero_range[hidx[rank]];
					if (((int)h.c1 == hero_c1 && (int)h.c2 == hero_c2) || ((int)h.c1 == hero_c2 && (int)h.c2 == hero_c1)) {
						std::printf("#%zu of %zu)\n", rank + 1, hidx.size());
						break;
					}
				}
			}
			std::printf("\nTop-15 tracked hero combos overall:\n");
			for (int k = 0; k < 15 && k < (int)hidx.size(); k++) {
				auto& h = hero_range[hidx[k]];
				std::printf("  #%2d %s%s = %.4f%%\n", k + 1,
					dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0);
			}
		}

		const char* hero_out_path = "/tmp/hero_river_range_kcflush.txt";
		FILE* f = std::fopen(hero_out_path, "w");
		if (f) {
			bool first = true;
			for (auto& h : hero_range) {
				if (!first) std::fputc(',', f);
				first = false;
				std::fprintf(f, "%s%s:%.8f", dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight);
			}
			std::fclose(f);
			std::printf("\n[DUMP] wrote %zu combos in TexasSolver range syntax to %s\n",
				hero_range.size(), hero_out_path);
		}
	}

	// --- Timing test: how long does DecisionHoldem's OWN production river
	// resolver (run_until_converged w/ RIVER's real ConvergenceConfig: batch=500,
	// max_iterations=20000, max_ms=6000) take on THIS exact board/pot with the
	// final, fully-narrowed hero_range (1081 combos) and g.villain_range (990
	// combos) as external_reach0/external_reach1 -- i.e. a real, both-sides
	// multi-combo river resolve identical in spirit to what live play would run.
	// This is a direct empirical measurement for comparing against TexasSolver's
	// wall-clock time on an equivalent river spot.
	{
		std::printf("\n=== TIMING: production river LiveResolver on final ranges ===\n");
		try {
			Searchstate s = build_current_searchstate(g.my_id);
			std::vector<std::array<unsigned char, 2>> hero_hands, villain_hands;
			hero_hands.reserve(hero_range.size());
			for (auto& h : hero_range) hero_hands.push_back({ h.c1, h.c2 });
			villain_hands.reserve(g.villain_range.size());
			for (auto& h : g.villain_range) villain_hands.push_back({ h.c1, h.c2 });

			Players_range range;
			range.hero = hero_hands;
			range.villain = villain_hands;

			std::vector<double> hero_weights, villain_weights;
			hero_weights.reserve(hero_range.size());
			for (auto& h : hero_range) hero_weights.push_back(h.weight);
			villain_weights.reserve(g.villain_range.size());
			for (auto& h : g.villain_range) villain_weights.push_back(h.weight);

			// full_ladder=true matches resolve_decision()'s REAL usage for RIVER
			// (dh_native_ai.cpp ~line 1206-1210): widens hero's own opening action
			// set to the real native pot-fraction bet ladder (0.5/1/2/4/10/20x pot)
			// instead of just fold/call/allin, and raises run_until_converged's
			// max_ms cap from 6000 to 10000. The FIRST version of this timing block
			// omitted full_ladder (left it at its false default) -- a materially
			// SMALLER/cheaper tree than what a real decision actually solves, which
			// is why it measured ~220ms while real play visibly takes seconds.
			bool full_ladder = true;
			auto t0 = std::chrono::steady_clock::now();
			LiveResolver resolver(range, engine, nullptr, LiveResolver::Mode::RIVER, false, nullptr, full_ladder);
			resolver.init_root(s, g.board);
			run_until_converged(resolver, LiveResolver::Mode::RIVER, &hero_weights, &villain_weights, full_ladder);
			double pot = (double)resolver.root->state.table.total_pot;
			double expl_pct = (pot > 1e-9)
				? 100.0 * resolver.exploitability(&hero_weights, &villain_weights) / pot
				: 0.0;
			auto t1 = std::chrono::steady_clock::now();
			double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

			std::printf("hero combos: %zu, villain combos: %zu\n", hero_hands.size(), villain_hands.size());
			std::printf("elapsed: %.1f ms, final exploitability: %.4f%% of pot\n", elapsed_ms, expl_pct);

			// --- Second timing test: opp_take_action() ALSO calls
			// narrow_villain_range_postflop() (extended_actions=true, full_ladder
			// left at its false default -- see dh_native_ai.cpp line 1065) for
			// EVERY observed villain action, including the very river bet that
			// puts hero to this decision. A real "hero facing a river bet" moment
			// therefore chains TWO separate resolves back to back: this narrowing
			// resolve first (triggered by villain's action), then resolve_decision()
			// (timed above) second (triggered by hero's own getdecision() call).
			// Time this second resolve, on the exact same ranges/board, to get the
			// real combined per-decision cost.
			std::printf("\n=== TIMING: opp_take_action's villain-narrowing resolve (chained before hero's decision) ===\n");
			auto t2 = std::chrono::steady_clock::now();
			LiveResolver narrow_resolver(range, engine, nullptr, LiveResolver::Mode::RIVER, /*extended_actions=*/true, nullptr);
			narrow_resolver.init_root(s, g.board);
			run_until_converged(narrow_resolver, LiveResolver::Mode::RIVER, &hero_weights, &villain_weights);
			auto t3 = std::chrono::steady_clock::now();
			double narrow_elapsed_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
			std::printf("elapsed: %.1f ms\n", narrow_elapsed_ms);
			std::printf("\n=== COMBINED (narrowing + decision) for one visible river decision: %.1f ms ===\n",
				narrow_elapsed_ms + elapsed_ms);

			// --- Third timing test: SAME board, SAME 1081/990 combo sets, but
			// UNIFORM weights instead of this hand's actual history-narrowed
			// weights. Isolates whether it was the WEIGHT DISTRIBUTION (not combo
			// COUNT -- 1081/990 is already the full board-pruned range size, see
			// C(47,2)=1081 and C(45,2)=990) that let this specific hand's resolve
			// converge in 368ms, vastly faster than dh_native_ai.cpp's own
			// documented "arbitrary synthetic full-range scenario" measurement of
			// RIVER+full_ladder only just crossing 1% (0.96%) AT the full 10000ms
			// cap. If uniform weights on this SAME board/combo-set also converge
			// fast, the board itself (paired, no-straight-possible per this file's
			// own header) is the reason. If uniform weights are much slower/hit
			// the cap, this specific hand's narrowed (concentrated) weights were
			// what made it easy, and real less-narrowed hands should be expected
			// to cost much closer to the full 10s cap, exactly as the user reports
			// seeing on every real river hand.
			std::printf("\n=== TIMING: SAME board/combos, UNIFORM weights (isolates weight-concentration effect) ===\n");
			std::vector<double> uniform_hero_weights(hero_hands.size(), 1.0 / (double)hero_hands.size());
			std::vector<double> uniform_villain_weights(villain_hands.size(), 1.0 / (double)villain_hands.size());
			auto t4 = std::chrono::steady_clock::now();
			LiveResolver uniform_resolver(range, engine, nullptr, LiveResolver::Mode::RIVER, false, nullptr, full_ladder);
			uniform_resolver.init_root(s, g.board);
			run_until_converged(uniform_resolver, LiveResolver::Mode::RIVER,
				&uniform_hero_weights, &uniform_villain_weights, full_ladder);
			double uniform_pot = (double)uniform_resolver.root->state.table.total_pot;
			double uniform_expl_pct = (uniform_pot > 1e-9)
				? 100.0 * uniform_resolver.exploitability(&uniform_hero_weights, &uniform_villain_weights) / uniform_pot
				: 0.0;
			auto t5 = std::chrono::steady_clock::now();
			double uniform_elapsed_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();
			std::printf("elapsed: %.1f ms, final exploitability: %.4f%% of pot\n", uniform_elapsed_ms, uniform_expl_pct);
		}
		catch (const std::exception& e) {
			std::fprintf(stderr, "[TIMING] river resolve failed: %s\n", e.what());
		}
	}

	return 0;
}
