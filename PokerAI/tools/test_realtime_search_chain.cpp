//###############################################################################
//   NEW, ORIGINAL test tool for RealtimeSearch.h's StreetChainResolver (see
//   that file's header comment for what this is/is not). Not part of
//   upstream DecisionHoldem.
//
//   Unlike test_realtime_search_flop.cpp (which stops at a static leaf-value
//   estimate the instant flop betting ends), this tool resolves a fixed flop
//   all the way through REAL turn and river chance nodes (actual card deals)
//   and REAL turn/river betting rounds, down to an EXACT showdown via
//   Engine::compute_winner() (sevencards_strength.bin) -- no cluster
//   approximation anywhere in this resolve.
//
//   Because chance nodes fan out over every undealt card (~49 turn cards,
//   each with ~48 river cards), this uses a DELIBERATELY SMALL hero/villain
//   range (6 combos each) to keep the resulting tree tractable for a first
//   working, measured version. See BUILD_NOTES.md section 16 for the
//   measured node count/time/RAM at this scale and the scaling math for
//   larger ranges.
//
//   Build (from PokerAI/, matching this repo's existing no-Makefile
//   convention -- see BUILD_NOTES.md):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER \
//         -o /tmp/test_realtime_search_chain tools/test_realtime_search_chain.cpp
//     cd PokerAI && /tmp/test_realtime_search_chain
//
//   DH_SKIP_RIVER_CLUSTER is the same opt-in, default-off macro documented in
//   BUILD_NOTES.md section 9. This tool's showdown terminal calls
//   compute_winner(), which only needs sevencards_strength.bin (already
//   loaded) -- it does NOT need river_hand_cluster.bin, so skipping that
//   ~16.86GB load keeps this within the host's RAM budget without affecting
//   normal (flag-undefined) behavior or this tool's correctness.
//###############################################################################
#include "../tree/RealtimeSearch.h"
#include <chrono>
#include <cstdio>

using namespace RealtimeSearch;

static void print_hand(const std::array<unsigned char, 2>& h) {
	printf("[%2d,%2d]", h[0], h[1]);
}

int main() {
	// `engine` (poker/State.h) is a global `Engine* engine = new Engine();`
	// constructed by a static initializer before main() runs.
	printf("Engine already constructed (global static init, before main()); proceeding.\n");

	unsigned char board[3] = { 10, 23, 41 }; // arbitrary fixed 3 distinct cards

	// Small explicit hero/villain range: 6 combos each, drawn from cards not
	// on the board, all mutually distinct within a hand. Deliberately small
	// (vs. 30x30 in the flop-only demo) because full turn+river chaining
	// multiplies node count by ~49*48 per completed flop betting line.
	Players_range range;
	{
		std::mt19937_64 rng(7);
		std::vector<unsigned char> deck;
		for (int c = 0; c < 52; c++)
			if (c != board[0] && c != board[1] && c != board[2]) deck.push_back((unsigned char)c);
		auto sample_range = [&](int want, std::vector<std::array<unsigned char, 2>>& out) {
			while ((int)out.size() < want) {
				int i = rng() % deck.size();
				int j = rng() % deck.size();
				if (i == j) continue;
				unsigned char a = deck[i], b = deck[j];
				if (a > b) std::swap(a, b);
				bool dup = false;
				for (auto& h : out) if (h[0] == a && h[1] == b) { dup = true; break; }
				if (!dup) out.push_back({ a, b });
			}
		};
		sample_range(6, range.hero);
		sample_range(6, range.villain);
	}
	printf("Hero range: %zu combos, Villain range: %zu combos, board=[%d,%d,%d]\n",
		range.hero.size(), range.villain.size(), board[0], board[1], board[2]);

	// Same 200bb-effective, single-raised-pot flop scenario as the flop-only
	// demo (see that tool's comment for the invariant explanation): pot=600,
	// stacks reduced to 19700 each, last_bigbet=300.
	Searchstate s0;
	s0.small_blind = 50;
	s0.big_blind = 100;
	s0.has_allin = false;
	s0.betting_stage = 1; // flop
	s0.table.players[0] = SearchPlayer(20000);
	s0.table.players[1] = SearchPlayer(20000);
	s0.table.players[0].n_chips = 19700;
	s0.table.players[1].n_chips = 19700;
	s0.table.total_pot = 600;
	s0.last_bigbet = 300;
	s0.reset_betting_round_state();

	StreetChainResolver resolver(range, engine);
	resolver.init_root(s0, board);

	auto avg_abs_regret = [](StreetChainResolver::Node* node) {
		double sum = 0.0;
		int cnt = 0;
		for (auto& row : node->regret) for (double r : row) { sum += std::fabs(r); cnt++; }
		return cnt ? sum / cnt : 0.0;
	};

	const int checkpoints[] = { 5, 15, 30, 50 };
	int done = 0;
	printf("Running vanilla-CFR over the flop->turn->river chained subgame...\n");
	printf("(fewer iterations than the flop-only demo -- each iteration now walks the FULL\n"
		" tree, including every turn/river chance branch, so each one is far more expensive.)\n");
	auto t_search_start = std::chrono::steady_clock::now();
	for (int target : checkpoints) {
		resolver.run(target - done);
		done = target;
		double r = avg_abs_regret(resolver.root.get());
		long long nodes = resolver.node_count();
		printf("  after %3d iterations: root avg|regret|=%8.1f  (%.3f/iter)  tree nodes so far=%lld\n",
			done, r, r / done, nodes);
	}
	auto t_search_end = std::chrono::steady_clock::now();
	double search_ms = std::chrono::duration<double, std::milli>(t_search_end - t_search_start).count();
	const int iterations = done;

	printf("\n=== Resolve complete ===\n");
	printf("CFR search time (excludes Engine load): %.1f ms for %d iterations (flop->turn->river, exact showdown)\n",
		search_ms, iterations);
	printf("Final tree size: %lld nodes (decision + chance)\n", resolver.node_count());

	printf("\nLegal root actions (player %d to act):", resolver.root->state.player_i_index);
	for (auto a : resolver.root->actions) printf(" %d", (int)a);
	printf("\n");

	int root_player = resolver.root->state.player_i_index;
	const auto& root_range = (root_player == 0) ? range.hero : range.villain;
	printf("\nRoot decision belongs to player %d (%s). Average strategy for all %zu hands:\n",
		root_player, root_player == 0 ? "hero" : "villain", root_range.size());
	for (int hi = 0; hi < (int)root_range.size(); hi++) {
		std::vector<double> avg;
		StreetChainResolver::average_strategy(resolver.root.get(), hi, avg);
		print_hand(root_range[hi]);
		printf(" ->");
		for (size_t a = 0; a < resolver.root->actions.size(); a++)
			printf(" action=%d:p=%.3f", (int)resolver.root->actions[a], avg[a]);
		printf("\n");
	}
	printf("\n(CFR sanity check: per-iteration average |regret| above should trend DOWN as iteration\n"
		" count grows. Fewer iterations than the flop-only demo are used here because each\n"
		" iteration is dramatically more expensive -- it now includes full turn+river dealing\n"
		" and an exact showdown at every terminal, not just a one-shot leaf estimate.)\n");

	return 0;
}
