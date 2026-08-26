//###############################################################################
//   test_live_resolver_iteration_budget.cpp -- measures the real wall-clock
//   cost of the CFR iteration budget dh_native_ai.cpp actually uses per live
//   decision (see run_iterations_for_mode() there): 6000 iterations for FLOP
//   mode, 10000 for RIVER mode, 300 for TURN mode. This is a companion to
//   test_live_resolver_range_scaling.cpp (which measured the *old* 60-
//   iteration budget across range sizes) -- this tool instead fixes the
//   range size at the realistic full ~1000-1225-combo case and measures the
//   NEW iteration budget's cost, to confirm it stays acceptably fast for
//   live play before shipping it.
//
//   Context: 60 iterations of vanilla CFR is a low convergence budget --
//   real Slumbot play showed a recurring pattern of the resolver
//   occasionally shoving all-in with only a marginal hand (e.g. bottom
//   pair) and losing the whole stack, consistent with average-strategy
//   noise from under-convergence. Since even the FULL range at 60
//   iterations cost well under 9ms (see test_live_resolver_range_scaling.cpp
//   / BUILD_NOTES.md section 25), there was ample headroom to raise the
//   budget substantially without hurting live-play responsiveness.
//
//   TURN mode is deliberately capped far lower than FLOP/RIVER (300, not
//   10000): unlike FLOP (leaf-model shortcut) and RIVER (final street, no
//   further chance node), every TURN-mode CFR iteration fully enumerates
//   all ~44 possible river cards as real chance-tree children -- this tool
//   is what caught that 10000 TURN iterations actually costs ~123.6
//   SECONDS per decision (measured), not a fraction of a second, before it
//   shipped. See run_iterations_for_mode()'s comment in dh_native_ai.cpp
//   for the full writeup and BUILD_NOTES.md for the real before/after
//   numbers.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_live_resolver_iteration_budget tools/test_live_resolver_iteration_budget.cpp
//   RUN (from PokerAI/):
//     ./tools/test_live_resolver_iteration_budget
//###############################################################################
#include "../tree/RealtimeSearch.h"
#include <cstdio>
#include <chrono>
#include <vector>
#include <array>

using namespace RealtimeSearch;

static void build_range(int count, unsigned char exclude1, unsigned char exclude2,
	std::vector<std::array<unsigned char, 2>>& out) {
	out.clear();
	for (int c = 0; c < 52 && (int)out.size() < count; c++) {
		if (c == exclude1 || c == exclude2) continue;
		for (int d = c + 1; d < 52 && (int)out.size() < count; d++) {
			if (d == exclude1 || d == exclude2) continue;
			out.push_back({ (unsigned char)c, (unsigned char)d });
		}
	}
}

int main() {
	unsigned char hero_c1 = 51, hero_c2 = 47;
	unsigned char flop_board[3] = { 0, 13, 26 }; // arbitrary distinct-rank board

	// FLOP mode: realistic full range (~1081 post-collision combos, same as
	// the largest case test_live_resolver_range_scaling.cpp measured),
	// timed at the new 6000-iteration budget.
	{
		std::vector<std::array<unsigned char, 2>> villain_range;
		build_range(1225, hero_c1, hero_c2, villain_range);
		std::vector<std::array<unsigned char, 2>> filtered;
		for (auto& h : villain_range) {
			bool collide = false;
			for (unsigned char b : flop_board) if (h[0] == b || h[1] == b) { collide = true; break; }
			if (!collide) filtered.push_back(h);
		}

		Players_range range;
		range.hero = { { hero_c1, hero_c2 } };
		range.villain = filtered;

		TurnClusterLeafModel leaf(engine, flop_board, range);
		Searchstate s;
		s.small_blind = 50; s.big_blind = 100;
		s.betting_stage = 1;
		s.table.players[0] = SearchPlayer(20000);
		s.table.players[1] = SearchPlayer(20000);
		s.table.players[0].n_chips = 19900;
		s.table.players[1].n_chips = 19900;
		s.table.total_pot = 200;
		s.last_bigbet = 100;
		s.player_i_index = 1;
		// A fresh Searchstate leaves has_allin/n_raises/cur_round_action_num/
		// last_raise as UNINITIALIZED garbage (Searchstate's constructor only
		// sets small_blind/big_blind) -- without this, legal_actions_river()/
		// legal_actions() silently degenerate to a single legal action ('l'
		// check/call) if has_allin happens to read as garbage-true, which is
		// exactly what happened here before this fix. Explicitly initialize a
		// genuine start-of-street state (mirrors Searchstate::reset_betting_
		// round_state() and dh_native_ai.cpp's build_current_searchstate()).
		s.has_allin = false;
		s.n_raises = 0;
		s.cur_round_action_num = 0;
		s.last_raise = 0;

		LiveResolver resolver(range, engine, &leaf, LiveResolver::Mode::FLOP);
		resolver.init_root(s, std::vector<unsigned char>(flop_board, flop_board + 3));

		auto t0 = std::chrono::steady_clock::now();
		resolver.run(6000);
		auto t1 = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		std::printf("FLOP  mode: villain range size=%4zu, run(6000)  took %8.1f ms\n", filtered.size(), ms);
	}

	// TURN/RIVER mode: no TurnClusterLeafModel needed (nullptr leaf, exactly
	// as dh_native_ai.cpp does for these modes), same realistic full range,
	// timed at the new 10000-iteration budget.
	for (auto mode : { LiveResolver::Mode::TURN, LiveResolver::Mode::RIVER }) {
		std::vector<std::array<unsigned char, 2>> villain_range;
		build_range(1225, hero_c1, hero_c2, villain_range);
		unsigned char board5[5] = { 0, 13, 26, 39, 2 }; // arbitrary 4-5 distinct-rank cards
		int n_board = (mode == LiveResolver::Mode::TURN) ? 4 : 5;
		std::vector<std::array<unsigned char, 2>> filtered;
		for (auto& h : villain_range) {
			bool collide = false;
			for (int i = 0; i < n_board; i++) if (h[0] == board5[i] || h[1] == board5[i]) { collide = true; break; }
			if (!collide) filtered.push_back(h);
		}

		Players_range range;
		range.hero = { { hero_c1, hero_c2 } };
		range.villain = filtered;

		Searchstate s;
		s.small_blind = 50; s.big_blind = 100;
		s.betting_stage = (mode == LiveResolver::Mode::TURN) ? 2 : 3;
		s.table.players[0] = SearchPlayer(20000);
		s.table.players[1] = SearchPlayer(20000);
		s.table.players[0].n_chips = 19900;
		s.table.players[1].n_chips = 19900;
		s.table.total_pot = 200;
		s.last_bigbet = 100;
		s.player_i_index = 1;
		// A fresh Searchstate leaves has_allin/n_raises/cur_round_action_num/
		// last_raise as UNINITIALIZED garbage (Searchstate's constructor only
		// sets small_blind/big_blind) -- without this, legal_actions_river()/
		// legal_actions() silently degenerate to a single legal action ('l'
		// check/call) if has_allin happens to read as garbage-true, which is
		// exactly what happened here before this fix. Explicitly initialize a
		// genuine start-of-street state (mirrors Searchstate::reset_betting_
		// round_state() and dh_native_ai.cpp's build_current_searchstate()).
		s.has_allin = false;
		s.n_raises = 0;
		s.cur_round_action_num = 0;
		s.last_raise = 0;

		LiveResolver resolver(range, engine, nullptr, mode);
		resolver.init_root(s, std::vector<unsigned char>(board5, board5 + n_board));

		auto t0 = std::chrono::steady_clock::now();
		int iters = (mode == LiveResolver::Mode::TURN) ? 300 : 10000;
		resolver.run(iters);
		auto t1 = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		std::printf("%s mode: villain range size=%4zu, run(%d) took %8.1f ms\n",
			mode == LiveResolver::Mode::TURN ? "TURN " : "RIVER", filtered.size(), iters, ms);
	}

	return 0;
}
