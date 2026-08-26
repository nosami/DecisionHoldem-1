//###############################################################################
//   test_run_until_converged.cpp -- end-to-end measurement of the adaptive
//   convergence loop dh_native_ai.cpp actually uses per live decision (see
//   run_until_converged()/convergence_config_for_mode() there): run CFR in
//   small batches, stop once measured exploitability drops under ~1% of the
//   pot or a safety cap (iteration count / wall-clock time) is hit. This
//   tool duplicates that same small piece of logic (rather than #include-ing
//   dh_native_ai.cpp, which also defines the C ABI exports and a global
//   LiveGame) so it can be measured standalone against the same realistic
//   full-range scenario used throughout this test-tool family.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_run_until_converged tools/test_run_until_converged.cpp
//   RUN (from PokerAI/):
//     ./tools/test_run_until_converged
//###############################################################################
#include "../tree/RealtimeSearch.h"
#include <cstdio>
#include <chrono>
#include <vector>
#include <array>
#include <algorithm>

using namespace RealtimeSearch;

struct ConvergenceConfig {
	int batch_size;
	int max_iterations;
	double max_ms;
};

static ConvergenceConfig convergence_config_for_mode(LiveResolver::Mode mode) {
	if (mode == LiveResolver::Mode::FLOP)  return { 200, 10000, 3000.0 };
	if (mode == LiveResolver::Mode::TURN)  return { 100, 2000, 12000.0 };
	return { 500, 20000, 6000.0 }; // RIVER
}

static const double TARGET_EXPLOITABILITY_PCT = 1.0;

// Returns { iterations run, final exploitability % of pot, total wall ms,
// ms spent inside resolver.run(), ms spent inside resolver.exploitability() }.
static std::tuple<int, double, double, double, double> run_until_converged(LiveResolver& resolver, LiveResolver::Mode mode) {
	ConvergenceConfig cfg = convergence_config_for_mode(mode);
	double pot = (double)resolver.root->state.table.total_pot;
	auto t0 = std::chrono::steady_clock::now();
	int done = 0;
	double expl_pct = 100.0;
	double run_ms = 0.0, expl_ms = 0.0;
	while (done < cfg.max_iterations) {
		int batch = std::min(cfg.batch_size, cfg.max_iterations - done);
		auto r0 = std::chrono::steady_clock::now();
		resolver.run(batch);
		auto r1 = std::chrono::steady_clock::now();
		run_ms += std::chrono::duration<double, std::milli>(r1 - r0).count();
		done += batch;
		expl_pct = (pot > 1e-9) ? 100.0 * resolver.exploitability() / pot : 0.0;
		auto r2 = std::chrono::steady_clock::now();
		expl_ms += std::chrono::duration<double, std::milli>(r2 - r1).count();
		double elapsed_ms = std::chrono::duration<double, std::milli>(r2 - t0).count();
		if (expl_pct < TARGET_EXPLOITABILITY_PCT) break;
		if (elapsed_ms >= cfg.max_ms) break;
	}
	double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return { done, expl_pct, total_ms, run_ms, expl_ms };
}

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

static void run_mode(const char* label, LiveResolver::Mode mode) {
	unsigned char hero_c1 = 51, hero_c2 = 47;
	std::vector<std::array<unsigned char, 2>> villain_range;
	build_range(1225, hero_c1, hero_c2, villain_range);
	unsigned char board5[5] = { 0, 14, 28, 42, 5 }; // 2s,3c,4d,5h,7s -- non-degenerate
	int n_board = (mode == LiveResolver::Mode::FLOP) ? 3 : (mode == LiveResolver::Mode::TURN) ? 4 : 5;

	std::vector<std::array<unsigned char, 2>> filtered;
	for (auto& h : villain_range) {
		bool collide = false;
		for (int i = 0; i < n_board; i++) if (h[0] == board5[i] || h[1] == board5[i]) { collide = true; break; }
		if (!collide) filtered.push_back(h);
	}

	Players_range range;
	range.hero = { { hero_c1, hero_c2 } };
	range.villain = filtered;

	std::unique_ptr<TurnClusterLeafModel> leaf;
	if (mode == LiveResolver::Mode::FLOP) {
		unsigned char flop_board[3] = { board5[0], board5[1], board5[2] };
		leaf.reset(new TurnClusterLeafModel(engine, flop_board, range));
	}

	Searchstate s;
	s.small_blind = 50; s.big_blind = 100;
	s.betting_stage = (mode == LiveResolver::Mode::FLOP) ? 1 : (mode == LiveResolver::Mode::TURN) ? 2 : 3;
	s.table.players[0] = SearchPlayer(20000);
	s.table.players[1] = SearchPlayer(20000);
	s.table.players[0].n_chips = 19900;
	s.table.players[1].n_chips = 19900;
	s.table.total_pot = 200;
	s.last_bigbet = 100;
	s.player_i_index = 1;
	s.has_allin = false;
	s.n_raises = 0;
	s.cur_round_action_num = 0;
	s.last_raise = 0;

	LiveResolver resolver(range, engine, leaf.get(), mode);
	resolver.init_root(s, std::vector<unsigned char>(board5, board5 + n_board));

	auto [iters, expl_pct, ms, run_ms, expl_ms] = run_until_converged(resolver, mode);
	std::printf("%-6s villain range=%4zu  iters_run=%6d  final_exploit=%7.3f%%  wall=%9.1fms  (run=%.1fms expl=%.1fms, expl=%.1f%% of total)  %s\n",
		label, filtered.size(), iters, expl_pct, ms, run_ms, expl_ms, 100.0 * expl_ms / ms,
		(expl_pct < TARGET_EXPLOITABILITY_PCT) ? "(converged under target)" : "(hit safety cap first)");
}

int main() {
	run_mode("FLOP", LiveResolver::Mode::FLOP);
	run_mode("RIVER", LiveResolver::Mode::RIVER);
	run_mode("TURN", LiveResolver::Mode::TURN);
	return 0;
}
