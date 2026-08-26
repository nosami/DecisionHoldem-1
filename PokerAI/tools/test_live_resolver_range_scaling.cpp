//###############################################################################
//   test_live_resolver_range_scaling.cpp -- measures the real, honest cost of
//   resolving LiveResolver (RealtimeSearch.h, used by dh_native_ai.cpp's
//   getdecision()) against a FULL, ~1000-hand tracked opponent range versus
//   the OLD fixed 40-hand uniform sample it replaces. See BUILD_NOTES.md's
//   range-model section for the design this validates.
//
//   This does not touch disk (no blueprint/cluster files needed for FLOP
//   mode -- TurnClusterLeafModel only needs turn_hand_cluster.bin, already
//   loaded once via the global `engine`), so it isolates the CFR-resolve
//   cost itself, not I/O.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o test_live_resolver_range_scaling tools/test_live_resolver_range_scaling.cpp
//   RUN (from PokerAI/):
//     ./test_live_resolver_range_scaling
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
	unsigned char flop_board[3] = { 0, 13, 26 }; // 2c 2d 2h-ish arbitrary distinct-rank board

	for (int target_size : { 40, 200, 500, 1000, 1225 }) {
		std::vector<std::array<unsigned char, 2>> villain_range;
		build_range(target_size, hero_c1, hero_c2, villain_range);
		// Drop any combo colliding with the board (mirrors real usage).
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
		s.table.players[0].n_chips = 19900; // both contributed 100 pre-flop, consistent w/ total_pot below
		s.table.players[1].n_chips = 19900;
		s.table.total_pot = 200;
		s.last_bigbet = 100;
		s.player_i_index = 1;

		LiveResolver resolver(range, engine, &leaf, LiveResolver::Mode::FLOP);
		resolver.init_root(s, std::vector<unsigned char>(flop_board, flop_board + 3));

		auto t0 = std::chrono::steady_clock::now();
		resolver.run(60);
		auto t1 = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		std::printf("villain range size=%4zu (requested %5d): resolver.run(60) took %8.1f ms\n",
			filtered.size(), target_size, ms);
	}
	return 0;
}
