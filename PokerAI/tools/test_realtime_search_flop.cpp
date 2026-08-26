//###############################################################################
//   NEW, ORIGINAL test tool for RealtimeSearch.h (see that file's header
//   comment for what this is/is not). Not part of upstream DecisionHoldem.
//
//   Sets up one concrete flop decision point (fixed board, fixed small
//   hero/villain ranges, fixed pot/stacks), builds the flop-only subgame via
//   Searchstate's real betting mechanics, runs the new vanilla range-vs-range
//   CFR resolver with a turn-cluster-comparison leaf model, and prints:
//     - the resulting average root strategy for a couple of hero hands
//     - a standard CFR sanity check (exploitability trend across iterations)
//     - wall-clock time for the resolve, separate from Engine construction
//
//   Build (from PokerAI/, matching this repo's existing no-Makefile
//   convention -- see BUILD_NOTES.md):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER \
//         -o /tmp/test_realtime_search_flop tools/test_realtime_search_flop.cpp
//     cd PokerAI && /tmp/test_realtime_search_flop
//
//   DH_SKIP_RIVER_CLUSTER is the same opt-in, default-off macro documented in
//   BUILD_NOTES.md section 9 -- this tool never calls get_river_cluster(), so
//   skipping the ~16.86GB river_hand_cluster.bin load keeps this within the
//   host's RAM budget without affecting normal (flag-undefined) behavior.
//###############################################################################
#include "../tree/RealtimeSearch.h"
#include <chrono>
#include <cstdio>

using namespace RealtimeSearch;

static void print_hand(const std::array<unsigned char, 2>& h) {
	printf("[%2d,%2d]", h[0], h[1]);
}

int main() {
	// NOTE: `engine` (poker/State.h) is a global `Engine* engine = new Engine();`,
	// constructed by a static initializer BEFORE main() runs, so its load time
	// isn't measurable from inside main(); it's already loaded by the time we
	// get here (preflop, flop, turn, sevencards cluster files; river skipped
	// under DH_SKIP_RIVER_CLUSTER -- see file header).
	printf("Engine already constructed (global static init, before main()); proceeding.\n");

	// Fixed flop board.
	unsigned char board[3] = { 10, 23, 41 }; // arbitrary fixed 3 distinct cards

	// Build a small explicit hero/villain range: 30 combos each, drawn from
	// cards not on the board, all mutually distinct within a hand.
	Players_range range;
	{
		std::mt19937_64 rng(42);
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
		sample_range(30, range.hero);
		sample_range(30, range.villain);
	}
	printf("Hero range: %zu combos, Villain range: %zu combos, board=[%d,%d,%d]\n",
		range.hero.size(), range.villain.size(), board[0], board[1], board[2]);

	printf("Precomputing turn-cluster leaf model...\n");
	auto t_leaf_start = std::chrono::steady_clock::now();
	TurnClusterLeafModel leaf(engine, board, range);
	auto t_leaf_end = std::chrono::steady_clock::now();
	printf("Leaf model precomputed in %.1f ms\n",
		std::chrono::duration<double, std::milli>(t_leaf_end - t_leaf_start).count());

	// Flop decision point: 200bb effective stacks (blinds 50/100), a single
	// preflop raise-to-300 + call already happened (pot=600, stacks reduced
	// to 19700 each). Values are illustrative, not tied to a specific hand
	// history beyond "a typical 200bb single-raised pot".
	Searchstate s0;
	s0.small_blind = 50;
	s0.big_blind = 100;
	s0.has_allin = false;
	s0.betting_stage = 1; // flop
	// Searchstate tracks CUMULATIVE (not per-street) chips committed via
	// n_bet_chips() = initial_chips - n_chips (see take_action()'s
	// n_chips_to_call formula and the table.total_pot == table.total()
	// invariant enforced by legal_actions()/take_action()). So a "start of
	// flop, pot already 600 from an equal preflop raise+call" scenario means:
	// initial_chips = original 20000 stack, n_chips = 19700 (300 already
	// committed each), and last_bigbet = 300 (the already-matched amount,
	// so nobody owes anything entering the flop). reset_betting_round_state()
	// does NOT reset last_bigbet -- by design it persists across streets.
	s0.table.players[0] = SearchPlayer(20000);
	s0.table.players[1] = SearchPlayer(20000);
	s0.table.players[0].n_chips = 19700;
	s0.table.players[1].n_chips = 19700;
	s0.table.total_pot = 600;
	s0.last_bigbet = 300;
	s0.reset_betting_round_state(); // player_i_index=1 (postflop, BB acts first), n_raises=0, last_raise=0

	FlopResolver resolver(range, leaf);
	resolver.init_root(s0);

	auto avg_abs_regret = [](FlopResolver::Node* node) {
		double sum = 0.0;
		int cnt = 0;
		for (auto& row : node->regret) for (double r : row) { sum += std::fabs(r); cnt++; }
		return cnt ? sum / cnt : 0.0;
	};

	// Run in checkpoints so we can show the standard CFR convergence
	// diagnostic (per-iteration average |regret| trending down) from a
	// SINGLE resolve, rather than restarting from scratch each time.
	const int checkpoints[] = { 25, 100, 300, 500 };
	int done = 0;
	printf("Running vanilla-CFR over the flop subgame, reporting convergence checkpoints...\n");
	auto t_search_start = std::chrono::steady_clock::now();
	for (int target : checkpoints) {
		resolver.run(target - done);
		done = target;
		double r = avg_abs_regret(resolver.root.get());
		printf("  after %4d iterations: root avg|regret|=%.1f  (%.3f per iteration)\n", done, r, r / done);
	}
	auto t_search_end = std::chrono::steady_clock::now();
	double search_ms = std::chrono::duration<double, std::milli>(t_search_end - t_search_start).count();
	const int iterations = done;

	printf("\n=== Resolve complete ===\n");
	printf("CFR search time (excludes Engine load + leaf-model precompute): %.1f ms for %d iterations\n",
		search_ms, iterations);

	printf("\nLegal root actions (player %d to act):", resolver.root->state.player_i_index);
	for (auto a : resolver.root->actions) printf(" %d", (int)a);
	printf("\n");

	// The root's acting player is whichever SearchTable index
	// reset_betting_round_state() assigned (1 = villain, since postflop the
	// non-button player acts first). Print villain's average root strategy
	// (that is the player actually deciding at the root node).
	int root_player = resolver.root->state.player_i_index;
	const auto& root_range = (root_player == 0) ? range.hero : range.villain;
	printf("\nRoot decision belongs to player %d (%s). Average strategy for its first 5 hands:\n",
		root_player, root_player == 0 ? "hero" : "villain");
	for (int hi = 0; hi < 5 && hi < (int)root_range.size(); hi++) {
		std::vector<double> avg;
		FlopResolver::average_strategy(resolver.root.get(), hi, avg);
		print_hand(root_range[hi]);
		printf(" ->");
		for (size_t a = 0; a < resolver.root->actions.size(); a++)
			printf(" action=%d:p=%.3f", (int)resolver.root->actions[a], avg[a]);
		printf("\n");
	}
	printf("\n(CFR sanity check: per-iteration average |regret| above should trend DOWN as iteration\n"
		" count grows -- cumulative regret itself keeps growing, just more slowly per iteration,\n"
		" which is the standard convergence signature of CFR-family algorithms.)\n");

	return 0;
}
