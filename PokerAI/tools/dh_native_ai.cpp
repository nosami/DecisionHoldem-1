//###############################################################################
//   dh_native_ai.cpp -- NEW, ORIGINAL native macOS replacement for the
//   Linux-only AlascasiaHoldem.so / blueprint.so binaries (which are ELF
//   shared objects and cannot load on Darwin/arm64 -- see BUILD_NOTES.md's
//   AlascasiaHoldem.so investigation section for the forensic detail on why
//   that binary cannot be ported or reconstructed).
//
//   This is a from-scratch, independently-implemented shared library that
//   exposes the EXACT SAME four-function C ABI that
//   pypokergui/server/fish_player_setup.py already calls via ctypes
//   (restart_game / Next_stage / opp_take_action / getdecision), so the
//   existing, UNMODIFIED Python GUI code can simply load this .dylib instead
//   of the .so on macOS. It is not a decompilation or reverse-engineering of
//   the original binary -- it is a new implementation built entirely on this
//   repo's own PokerAI/poker/*.h engine primitives and the new
//   PokerAI/tree/RealtimeSearch.h resolvers (FlopResolver's
//   TurnClusterLeafModel and the new LiveResolver class), decided upon by
//   inspecting the *calling* Python code's contract, not the .so's contents.
//
//   SCOPE / HONEST LIMITATIONS (see BUILD_NOTES.md section 17 for the full
//   writeup):
//     - PREFLOP is NOT solved. In the original DeepStack/Libratus/Alascasia
//       family of architectures (which this repo's own preflop_hand_cluster
//       + a since-unavailable 16GB blueprint_strategy.dat fit), preflop
//       decisions come from an offline-solved blueprint and only POSTFLOP
//       streets use real-time search. Without that blueprint, this library
//       always "call"s (or "check"s, functionally identical here) preflop --
//       a deliberate, clearly-labeled placeholder, not a solved strategy.
//     - FLOP/TURN/RIVER decisions use LiveResolver (RealtimeSearch.h): a
//       small, fast, REDUCED-ACTION (fold / call / all-in only -- no
//       intermediate bet sizes) range-vs-range vanilla CFR resolve against a
//       sampled, assumed-uniform opponent range (unsafe/fixed-range
//       resolving, like every resolver in this file; the opponent's actual
//       range is unknown and not tracked belief-state-style across the
//       hand). Turn decisions additionally assume the river gets checked
//       down (no river-betting subtree) purely to keep response times
//       interactive; river decisions are resolved exactly (real showdown,
//       no cluster approximation) since there are no more cards to deal.
//     - This is meant to make the existing GUI (pypokergui) ACTUALLY
//       PLAYABLE against a genuine, working, from-scratch search algorithm
//       on macOS -- it is explicitly NOT a reconstruction of the original
//       proprietary bot's strength or behavior.
//
//   Build (produces a macOS .dylib; run from PokerAI/ so the relative
//   "cluster/..." paths in Engine::load() resolve -- see BUILD_NOTES.md):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -shared -fPIC \
//         -o dh_native_ai.dylib tools/dh_native_ai.cpp
//
//   DH_SKIP_RIVER_CLUSTER (BUILD_NOTES.md section 9): this library's river
//   decisions/showdowns use Engine::compute_winner() (sevencards_strength.bin
//   only), never Engine::get_river_cluster(), so the ~16.86GB
//   river_hand_cluster.bin is not needed and is skipped for RAM.
//###############################################################################
#include "../tree/RealtimeSearch.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <random>
#include <algorithm>

using namespace RealtimeSearch;

namespace {

// ---------------------------------------------------------------------------
// Tracks just enough state, purely from the sequence of ABI calls the GUI
// makes, to reconstruct an accurate Searchstate snapshot at the moment a
// decision is actually needed. Internal slot convention (fixed, matches
// pypokergui/server/fish_player_setup.py's own myid derivation): slot 0 is
// always the small-blind/button seat, slot 1 is always the big-blind seat --
// this also matches Searchstate::reset_betting_round_state()'s own
// hardcoded HU convention (0 acts first preflop, 1 acts first postflop), so
// no extra translation is needed when handing a snapshot to a resolver.
// ---------------------------------------------------------------------------
struct LiveGame {
	int my_id = -1; // which slot (0 or 1) is "me" this hand
	int stack[2] = { 20000, 20000 };
	int stack_at_street_start[2] = { 20000, 20000 };
	int betting_stage = 0;
	int n_raises_this_street = 0;
	int actions_this_street = 0;
	int last_raise_size = 0;
	bool has_allin = false;
	int folder = -1; // slot that folded, or -1
	unsigned char my_hole[2] = { 0, 0 };
	std::vector<unsigned char> board;
	std::mt19937_64 rng{ std::random_device{}() };
};

LiveGame g;

int committed_this_street(int slot) {
	return g.stack_at_street_start[slot] - g.stack[slot];
}

void reset_street_counters() {
	g.stack_at_street_start[0] = g.stack[0];
	g.stack_at_street_start[1] = g.stack[1];
	g.n_raises_this_street = 0;
	g.actions_this_street = 0;
	g.last_raise_size = 0;
}

// Samples a small representative opponent range: `count` random 2-card hands
// drawn from cards not on the known board and not equal to hero's own hole
// cards, distinct from each other. This is the same "unsafe resolving,
// assumed-uniform opponent range" approach documented for every resolver in
// RealtimeSearch.h -- the opponent's true range is unknown and unavailable
// (no belief-tracking blueprint), so a uniform sample stands in for it.
void sample_opponent_range(int count, std::vector<std::array<unsigned char, 2>>& out) {
	std::vector<unsigned char> deck;
	for (int c = 0; c < 52; c++) {
		if (c == g.my_hole[0] || c == g.my_hole[1]) continue;
		bool on_board = false;
		for (unsigned char b : g.board) if (b == c) { on_board = true; break; }
		if (on_board) continue;
		deck.push_back((unsigned char)c);
	}
	out.clear();
	int guard = 0;
	while ((int)out.size() < count && guard < count * 50 + 200) {
		guard++;
		int i = g.rng() % deck.size();
		int j = g.rng() % deck.size();
		if (i == j) continue;
		unsigned char a = deck[i], b = deck[j];
		if (a > b) std::swap(a, b);
		bool dup = false;
		for (auto& h : out) if (h[0] == a && h[1] == b) { dup = true; break; }
		if (!dup) out.push_back({ a, b });
	}
}

// Builds the Searchstate snapshot for the current decision, runs the
// appropriately-scoped LiveResolver, and returns "fold" / "call" / "allin"
// sampled from hero's (my_id's) average strategy for my actual hole cards.
std::string resolve_decision() {
	Searchstate s;
	s.small_blind = 50;
	s.big_blind = 100;
	s.has_allin = g.has_allin;
	s.betting_stage = (unsigned char)g.betting_stage;
	s.table.players[0] = SearchPlayer(20000);
	s.table.players[1] = SearchPlayer(20000);
	s.table.players[0].n_chips = g.stack[0];
	s.table.players[1].n_chips = g.stack[1];
	s.table.total_pot = (20000 - g.stack[0]) + (20000 - g.stack[1]);
	s.last_bigbet = (unsigned short)std::max(20000 - g.stack[0], 20000 - g.stack[1]);
	s.player_i_index = (unsigned char)g.my_id;
	s.n_raises = (unsigned char)std::min(g.n_raises_this_street, 255);
	s.cur_round_action_num = (unsigned short)g.actions_this_street;
	s.first_action_of_current_round = (g.actions_this_street == 0) ? 1 : 0;
	s.last_raise = (unsigned short)g.last_raise_size;

	Players_range range;
	const int OPP_RANGE_SIZE = 40;
	std::vector<std::array<unsigned char, 2>> opp_range;
	sample_opponent_range(OPP_RANGE_SIZE, opp_range);
	std::array<unsigned char, 2> my_hand = { g.my_hole[0], g.my_hole[1] };

	if (g.my_id == 0) { range.hero = { my_hand }; range.villain = opp_range; }
	else { range.hero = opp_range; range.villain = { my_hand }; }

	LiveResolver::Mode mode = (g.betting_stage == 1) ? LiveResolver::Mode::FLOP
		: (g.betting_stage == 2) ? LiveResolver::Mode::TURN
		: LiveResolver::Mode::RIVER;

	std::unique_ptr<TurnClusterLeafModel> leaf;
	if (mode == LiveResolver::Mode::FLOP) {
		unsigned char flop_board[3] = { g.board[0], g.board[1], g.board[2] };
		leaf.reset(new TurnClusterLeafModel(engine, flop_board, range));
	}

	LiveResolver resolver(range, engine, leaf.get(), mode);
	resolver.init_root(s, g.board);
	resolver.run(60); // small iteration budget -- keeps live decisions fast (see BUILD_NOTES.md 17)

	// my_hand was placed at index 0 of whichever range list corresponds to
	// my slot (see above), and the root's acting player is always me (that
	// is the whole reason getdecision() was called), so index 0 always
	// addresses my own hand's strategy regardless of which slot I'm in.
	std::vector<double> avg;
	LiveResolver::average_strategy(resolver.root.get(), 0, avg);

	// Root actions may not literally be [fold, call, allin] in that order or
	// even all present (e.g., no fold offered if nothing is owed) -- match
	// by the actual encoded action bytes ('d'=fold, 'l'=call/check, 'n'=allin).
	double r = std::uniform_real_distribution<double>(0.0, 1.0)(g.rng);
	double cum = 0.0;
	for (size_t a = 0; a < resolver.root->actions.size(); a++) {
		cum += avg[a];
		if (r <= cum || a + 1 == resolver.root->actions.size()) {
			unsigned char act = resolver.root->actions[a];
			if (act == 'd') return "fold";
			if (act == 'n') return "allin";
			return "call";
		}
	}
	return "call"; // defensive fallback, should be unreachable
}

void apply_own_action(const std::string& action) {
	int me = g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	if (action == "fold") {
		g.folder = me;
		g.betting_stage = 5;
	}
	else if (action == "allin") {
		g.stack[me] = 0;
		g.has_allin = true;
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else { // "call" (also covers "check" -- identical bookkeeping when nothing is owed)
		g.stack[me] = g.stack_at_street_start[me] - prev_facing;
		g.actions_this_street++;
	}
}

} // namespace

extern "C" {

void restart_game(int myid, int c1id, int c2id) {
	g.my_id = myid;
	g.stack[0] = 20000 - 50;  // slot 0 = SB, always posts 50
	g.stack[1] = 20000 - 100; // slot 1 = BB, always posts 100
	g.betting_stage = 0;
	g.has_allin = false;
	g.folder = -1;
	g.my_hole[0] = (unsigned char)c1id;
	g.my_hole[1] = (unsigned char)c2id;
	g.board.clear();
	reset_street_counters();
}

void Next_stage(int betting_stage, char* community_card_idx) {
	int lenc = betting_stage + 2;
	g.board.assign((unsigned char*)community_card_idx, (unsigned char*)community_card_idx + lenc);
	g.betting_stage = betting_stage;
	reset_street_counters();
}

void opp_take_action(char* actionstr_c) {
	std::string a(actionstr_c);
	int opp = 1 - g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	if (a == "fold") {
		g.folder = opp;
		g.betting_stage = 5;
	}
	else if (a == "allin") {
		g.stack[opp] = 0;
		g.has_allin = true;
		int amount = g.stack_at_street_start[opp];
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else if (a.rfind("raise ", 0) == 0) {
		int amount = std::stoi(a.substr(6));
		g.stack[opp] = g.stack_at_street_start[opp] - amount;
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else { // "call" or "check"
		g.stack[opp] = g.stack_at_street_start[opp] - prev_facing;
		g.actions_this_street++;
	}
}

void getdecision(char* out_buf) {
	std::memset(out_buf, 0, 20);
	std::string action;
	if (g.betting_stage == 0) {
		// Documented simplification -- see file header. No preflop solve.
		action = "call";
	}
	else {
		action = resolve_decision();
	}
	apply_own_action(action);
	std::strncpy(out_buf, action.c_str(), 19);
}

} // extern "C"
