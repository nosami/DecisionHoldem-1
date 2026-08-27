//###############################################################################
//
//   This file is a NEW, ORIGINAL addition to the DecisionHoldem repository,
//   written from scratch. It is NOT a recovery, decompilation, or transcription
//   of the proprietary/withheld real-time search source (the code behind
//   AlascasiaHoldem.so / blueprint.so, e.g. a "Depth_limit_Search.h",
//   "Search.h" or "PlaySearch.h" that never shipped in this public repo -- see
//   BUILD_NOTES.md sections 12-14 for the forensic analysis that established
//   those files/functions are genuinely absent from the source tree).
//
//   It implements a published family of technique -- depth-limited /
//   subgame-resolving Counterfactual Regret Minimization, in the style of
//   Modicum/DeepStack/Libratus-class real-time search (Brown & Sandholm 2017;
//   Moravčík et al. 2017; Zinkevich et al. 2007 for vanilla CFR itself) --
//   using this repository's OWN, already-public building blocks:
//     - poker/State.h's Searchstate for real betting mechanics
//       (legal_actions()/take_action()/betting_stage), reused UNMODIFIED.
//     - poker/Engine.h's cluster lookups (get_turn_cluster, etc.), reused
//       UNMODIFIED, over the ALREADY-PUBLIC, ALREADY-DOCUMENTED
//       turn_hand_cluster.bin file.
//     - The same "compare ordinal hand-cluster ids to estimate relative hand
//       strength" idea already used (for a different, offline purpose) in
//       tree/Exploitability.h's getnode_cfv_river/turn/holdem.
//
//   Distributed under the same license as the rest of this repository:
//
//   Licensed under the GNU AFFERO GENERAL PUBLIC LICENSE
//                 Version 3, 19 November 2007
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//   GNU Affero General Public License for more details.
//
//   You should have received a copy of the GNU Affero General Public License
//   along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//###############################################################################
//
// SCOPE / HONEST LIMITATIONS (see BUILD_NOTES.md section 15 for the full
// writeup):
//   - This solves ONE flop betting round only (root = start of flop betting).
//   - It does NOT deal/solve the turn or river. When the flop betting round
//     ends (call, or all chips go in) without a fold, a static LEAF VALUE
//     model (TurnClusterLeafModel below) estimates the continuation value by
//     comparing each side's turn-hand-cluster id, averaged over the possible
//     next (turn) card -- a deliberately simple, RAM-cheap substitute for
//     the original, presumably blueprint-continuation-based, leaf value
//     (that file's own code shows subgame_node::leaf/leafnode pointing into
//     a loaded *blueprint* tree -- see tree/Bulid_Tree.h -- but that
//     blueprint_strategy.dat is 16.1GB, too large for this host's RAM
//     alongside the cluster files, so it is intentionally NOT used here).
//   - Hero/villain hole-card ranges are supplied explicitly as small,
//     concrete lists of 2-card combos (not the full unabstracted 1326-combo
//     range) -- this keeps runtimes small and avoids needing this repo's
//     production-scale range-abstraction machinery.
//   - Uses vanilla (full-traversal) CFR, not the original's evident
//     Monte-Carlo sampling (its function is literally named search_mccfr) --
//     simpler and lower bug-risk for a small subgame, at the cost of not
//     matching the original's sampling strategy.
//   - No safe/"gadget game" resolving (Burch et al.) is implemented --
//     like Searchstate's own preflopset()/setprivate_publiccards() comments
//     ("设置不安全搜索手牌" = "set unsafe-search hole cards"), this uses
//     unsafe resolving (fixed opponent range, no alternative-payoff gadget).
//
// UPDATE (see BUILD_NOTES.md section 16): turn/river chaining has since been
// added (StreetChainResolver, below) -- it deals real turn/river cards via
// chance nodes and resolves all the way to an EXACT showdown (Engine's
// compute_winner()/sevencards_strength.bin, not a cluster approximation),
// rather than stopping at a static flop-leaf estimate. FlopResolver (the
// flop-only, turn-cluster-leaf-estimate version documented above and in
// section 15) is kept as-is/unmodified for anyone who wants the cheaper,
// approximate, single-street version; it is not superseded, just joined by
// a more complete (and combinatorially far more expensive) sibling.
//
///////////////////////////////////////////////////////////////////////////////
#pragma once
#include "../poker/State.h"
#include "../poker/Engine.h"
#include <vector>
#include <array>
#include <random>
#include <cmath>
#include <memory>
#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <exception>
// Live-resolve action/chance-card fan-out parallelism uses OpenMP (the same
// mechanism $HOME/src/TexasSolver uses -- see BUILD_NOTES.md for the
// comparison), rather than a std::async-per-call thread spawn: OpenMP keeps a
// persistent worker thread pool alive for the whole process, so dispatching
// a parallel loop is a lightweight fork-join wakeup instead of paying full OS
// thread creation/destruction cost on every single call (which was measured
// to cap the earlier std::async version's TURN speedup at ~2.2x despite
// bursting to 35+ threads on a 10-core machine). When built WITHOUT the
// OpenMP compiler flags (no `-fopenmp`), _OPENMP is undefined and every
// parallel_map() call below falls back to the original plain serial loop --
// this keeps existing build commands that don't pass the new flags working
// unchanged, just without the speedup.
#ifdef _OPENMP
#include <omp.h>
#endif

namespace RealtimeSearch {

// ---------------------------------------------------------------------------
// Action translation: the pseudo-harmonic mapping of Ganzfried & Sandholm,
// "Action Translation in Extensive-Form Games with Large Action Spaces:
// A Harmonic Approach" (2013). Given an off-tree bet size x (pot fraction)
// bracketed by two on-tree sizes a <= x <= b, this returns the probability
// of mapping to the LOWER bracket size 'a':
//     f(a) = ((b - x) * (1 + a)) / ((b - a) * (1 + x))
// (mapping to 'b' with the complementary probability). This is a standalone,
// well-documented published formula -- it does not depend on anything
// proprietary and is unit-testable in isolation from cluster data.
// ---------------------------------------------------------------------------
inline double pseudo_harmonic_prob_lower(double a, double b, double x) {
	if (b <= a) return 1.0;
	if (x <= a) return 1.0;
	if (x >= b) return 0.0;
	double num = (b - x) * (1.0 + a);
	double den = (b - a) * (1.0 + x);
	if (den <= 0) return 1.0;
	double p = num / den;
	if (p < 0) p = 0;
	if (p > 1) p = 1;
	return p;
}

template <typename RNG>
inline bool randomized_pseudo_harmonic(double a, double b, double x, RNG& rng) {
	double p_lower = pseudo_harmonic_prob_lower(a, b, x);
	std::uniform_real_distribution<double> u(0.0, 1.0);
	return u(rng) < p_lower;
}

// A concrete hero/villain range: explicit lists of 2-card hole-card combos
// (card ids 0-51, this repo's native convention -- see poker/Card.h).
// NOT the same as the .so's opaque "Players_range" struct (whose internal
// layout was never recovered -- see BUILD_NOTES.md section 14); this is a
// new, independent type with the same descriptive name.
struct Players_range {
	std::vector<std::array<unsigned char, 2>> hero;
	std::vector<std::array<unsigned char, 2>> villain;
};

// Two 2-card hole-card hands are only simultaneously possible if they share
// no physical card. Independently-sampled hero/villain ranges can (and, in
// practice, sometimes do) contain such mutually-impossible combos; any
// showdown-type computation MUST skip them (not just skip cards colliding
// with the board), both for correctness (an impossible pair should never
// contribute mass to the counterfactual value sum) and, for the exact
// showdown model below, to avoid feeding Engine::compute_winner()/
// find_strength() a 7-card key that can never occur in the lookup table.
inline bool hands_compatible(const std::array<unsigned char, 2>& a, const std::array<unsigned char, 2>& b) {
	return a[0] != b[0] && a[0] != b[1] && a[1] != b[0] && a[1] != b[1];
}

// Standard regret-matching (same formula as tree/Node.h's calculate_strategy,
// reimplemented locally so this header has no dependency on strategy_node's
// raw-pointer array conventions, which were built for a different -- batch,
// pointer-tree-based -- use case).
inline void regret_matching(const std::vector<double>& regret, std::vector<double>& out_strategy) {
	int n = (int)regret.size();
	out_strategy.assign(n, 0.0);
	double pos_sum = 0.0;
	for (int a = 0; a < n; a++) {
		double r = regret[a] > 0 ? regret[a] : 0.0;
		out_strategy[a] = r;
		pos_sum += r;
	}
	if (pos_sum > 1e-9) {
		for (int a = 0; a < n; a++) out_strategy[a] /= pos_sum;
	}
	else {
		for (int a = 0; a < n; a++) out_strategy[a] = 1.0 / n;
	}
}

// ---------------------------------------------------------------------------
// Leaf-value model -- see the file-level comment above for what this is and
// is not. Precomputes, once, each range hand's turn-hand-cluster id against
// every possible (non-colliding) next card, using Engine::get_turn_cluster
// over the already-loaded turn_hand_cluster.bin. Leaf evaluation is then a
// cheap table lookup + ordinal comparison, averaged over turn cards that
// don't collide with either side's hole cards.
// ---------------------------------------------------------------------------
class TurnClusterLeafModel {
public:
	TurnClusterLeafModel(Engine* eng, const unsigned char board[3], const Players_range& range)
		: engine(eng), range_(range) {
		board_[0] = board[0]; board_[1] = board[1]; board_[2] = board[2];
		for (int c = 0; c < 52; c++) {
			if (c == board_[0] || c == board_[1] || c == board_[2]) continue;
			candidates.push_back((unsigned char)c);
		}
		precompute(range_.hero, hero_clusters);
		precompute(range_.villain, villain_clusters);
	}

	// Average showdown sign (+1 hero wins / -1 hero loses / 0 tie) for hero
	// hand index hi vs villain hand index vi, averaged over all sampled turn
	// cards that don't collide with either hand's hole cards.
	//
	// POLARITY (found via a real live-play bug, see BUILD_NOTES.md section
	// 22): a LOWER `get_turn_cluster()` id is the STRONGER hand, not a
	// higher one. This was verified two independent ways: (1) the
	// original authors' own offline `tree/Exploitability.h::getnode_cfv_river()`
	// treats `clusters[mycard] > clusters[j]` as hero LOSING; (2) a direct
	// empirical test against the real `turn_hand_cluster.bin` for a known
	// weak hand (ten-high + a gutshot on a random flop) found the old
	// `hc > vc` convention here reported hero as a ~76% favorite, while
	// real Monte-Carlo equity for that exact spot is ~33% -- i.e. the old
	// code had the comparison backwards for every flop decision that
	// reaches this leaf model, not just one hand.
	double expected_showdown_sign(int hi, int vi) const {
		const auto& hh = range_.hero[hi];
		const auto& vh = range_.villain[vi];
		double sum = 0.0;
		int n = 0;
		for (size_t k = 0; k < candidates.size(); k++) {
			unsigned char c = candidates[k];
			if (c == hh[0] || c == hh[1] || c == vh[0] || c == vh[1]) continue;
			int hc = hero_clusters[hi][k];
			int vc = villain_clusters[vi][k];
			if (hc < 0 || vc < 0) continue; // collided with own hole cards
			if (hc < vc) sum += 1.0;      // lower cluster id = stronger hand = hero wins
			else if (hc > vc) sum -= 1.0;
			n++;
		}
		if (n == 0) return 0.0;
		return sum / n;
	}

private:
	void precompute(const std::vector<std::array<unsigned char, 2>>& hands, std::vector<std::vector<int>>& out) {
		out.assign(hands.size(), std::vector<int>(candidates.size(), -1));
		for (size_t hi = 0; hi < hands.size(); hi++) {
			unsigned char h[2] = { hands[hi][0], hands[hi][1] };
			if (h[0] == board_[0] || h[0] == board_[1] || h[0] == board_[2] ||
				h[1] == board_[0] || h[1] == board_[1] || h[1] == board_[2]) continue;
			for (size_t k = 0; k < candidates.size(); k++) {
				unsigned char c = candidates[k];
				if (c == h[0] || c == h[1]) continue;
				unsigned char com[4] = { board_[0], board_[1], board_[2], c };
				out[hi][k] = (int)engine->get_turn_cluster(h, com);
			}
		}
	}

	Engine* engine;
	const Players_range& range_;
	unsigned char board_[3];
	std::vector<unsigned char> candidates;
	std::vector<std::vector<int>> hero_clusters;    // [hand_idx][candidate_idx] -> turn cluster id, or -1
	std::vector<std::vector<int>> villain_clusters;
};

// ---------------------------------------------------------------------------
// RiverClusterLeafModel -- the TURN-mode analogue of TurnClusterLeafModel
// above, giving LiveResolver's TURN mode the same kind of cheap terminal
// shortcut FLOP mode already has, instead of dealing a real river chance
// node (~48 branches) and resolving an exact showdown for every one every
// single CFR iteration (see BUILD_NOTES.md section 29 for why that was the
// measured cost driver behind TURN mode being much slower than FLOP mode).
//
// The blocker documented in section 29 was RAM: Engine::get_river_cluster()
// requires the entire 16.86GB river_hand_cluster.bin resident in memory,
// which does not fit this host's 16GB. This class does NOT use
// Engine::get_river_cluster() or the monolithic file at all. Instead it
// reads directly from the PER-HOLE-HAND SPLIT files built in section 31
// (one ~12.7MB file per of the 1326 possible hole hands, `<handid>.bin` =
// `[keys: 2,118,760 x 4-byte unsigned, ascending]` followed by
// `[values: 2,118,760 x 2-byte unsigned short]`, index-aligned with keys --
// exactly Engine.h's own in-RAM layout, just one hand's slice of it, on
// disk instead of in RAM).
//
// The key enabling result (section 34, validated 300/300 against real
// split files in tools/test_river_rank_seek.cpp before this class was
// written): because Engine.h's river key is a base-52 positional encoding
// of the 5-card board sorted ascending by raw card index, and the encoding
// is unaffected in RELATIVE order by which 2 raw values are excluded (the
// hand's own hole cards), sorting all C(50,5) possible boards by key is
// IDENTICAL to standard lexicographic order of the 5-tuple over the
// 50-card universe that remains after removing the hand's 2 hole cards.
// This means the row (rank) of any specific board within a hand's sorted
// keys[] array is computable via a closed-form combinatorial formula --
// no full-file load, no binary search, no scan: one `pread()` of 2 bytes
// per candidate river card, at a directly-computed byte offset.
// ---------------------------------------------------------------------------
class RiverClusterLeafModel {
public:
	static const long RIVER_COMMUNITY_TOTAL = 2118760;
	static const long VALUES_OFFSET = RIVER_COMMUNITY_TOTAL * 4;

	// `split_dir`: path to the directory of per-hole-hand split files
	// (`<h1*52+h2>.bin`, h1<h2). If this directory/files can't be opened,
	// the model marks itself unavailable (available()==false) rather than
	// throwing or fabricating data -- callers must check available() and
	// fall back to the existing exact chance-node+showdown resolve when
	// false (see LiveResolver::cfr()/best_response(), which do exactly
	// this).
	RiverClusterLeafModel(const std::string& split_dir, const unsigned char board[4], const Players_range& range)
		: range_(range), split_dir_(split_dir) {
		board_[0] = board[0]; board_[1] = board[1]; board_[2] = board[2]; board_[3] = board[3];
		build_binomial_table();
		for (int c = 0; c < 52; c++) {
			if (c == board_[0] || c == board_[1] || c == board_[2] || c == board_[3]) continue;
			candidates.push_back((unsigned char)c);
		}
		// Probe availability with the first hand we can find on either
		// side before committing to a full precompute pass -- avoids
		// silently doing nothing useful (every lookup falling through to
		// -1) when the split directory simply isn't present on this host.
		available_ = probe_available();
		if (!available_) {
			std::fprintf(stderr,
				"[RiverClusterLeafModel] split directory '%s' unavailable/unreadable -- "
				"TURN-mode leaf shortcut disabled for this decision, falling back to exact "
				"chance-node resolve\n", split_dir_.c_str());
			return;
		}
		precompute(range_.hero, hero_clusters);
		precompute(range_.villain, villain_clusters);
	}

	bool available() const { return available_; }

	// Same polarity convention as TurnClusterLeafModel::expected_showdown_sign
	// (lower cluster id = stronger hand), confirmed for RIVER clusters
	// specifically via the ORIGINAL, unmodified tree/Exploitability.h's
	// getnode_cfv_river(): `if (clusters[mycard] > clusters[j])
	// actionicfvs1[j] = -pot*0.5;` -- i.e. a higher river-cluster id for
	// the traversing player's hand than the opponent's is a LOSS, so
	// lower id = stronger hand, exactly like turn clusters (see BUILD_NOTES.md
	// section 34).
	double expected_showdown_sign(int hi, int vi) const {
		const auto& hh = range_.hero[hi];
		const auto& vh = range_.villain[vi];
		double sum = 0.0;
		int n = 0;
		for (size_t k = 0; k < candidates.size(); k++) {
			unsigned char c = candidates[k];
			if (c == hh[0] || c == hh[1] || c == vh[0] || c == vh[1]) continue;
			int hc = hero_clusters[hi][k];
			int vc = villain_clusters[vi][k];
			if (hc < 0 || vc < 0) continue; // collided with own hole cards, or lookup unavailable
			if (hc < vc) sum += 1.0;      // lower cluster id = stronger hand = hero wins
			else if (hc > vc) sum -= 1.0;
			n++;
		}
		if (n == 0) return 0.0;
		return sum / n;
	}

private:
	static uint64_t C_TABLE[53][6];
	static bool binomial_built_;

	static void build_binomial_table() {
		if (binomial_built_) return;
		for (int n = 0; n <= 52; n++)
			for (int k = 0; k <= 5; k++) {
				if (k == 0 || k == n) C_TABLE[n][k] = 1;
				else if (k > n) C_TABLE[n][k] = 0;
				else C_TABLE[n][k] = 0;
			}
		for (int n = 1; n <= 52; n++)
			for (int k = 1; k <= 5 && k < n; k++)
				C_TABLE[n][k] = C_TABLE[n - 1][k - 1] + C_TABLE[n - 1][k];
		binomial_built_ = true;
	}

	static uint64_t binom(int n, int k) {
		if (k < 0 || n < 0 || k > n) return 0;
		return C_TABLE[n][k];
	}

	// Standard lex-rank ("successor counting") of a k-combination, over a
	// contiguous universe {0,...,n-1} -- see tools/test_river_rank_seek.cpp
	// for the hand-derived/validated derivation of this exact formula
	// (deliberately NOT the more commonly-documented colex/combinatorial-
	// number-system formula, which gives the wrong order for this file's
	// key convention).
	static uint64_t lex_rank_of_combination(const int* c, int k, int n) {
		uint64_t rank = 0;
		int prev = -1;
		for (int i = 0; i < k; i++) {
			for (int v = prev + 1; v < c[i]; v++)
				rank += binom(n - 1 - v, k - 1 - i);
			prev = c[i];
		}
		return rank;
	}

	static int compact_index(int raw, int h1, int h2) {
		int shift = 0;
		if (raw > h1) shift++;
		if (raw > h2) shift++;
		return raw - shift;
	}

	// Tests whether the split directory is actually usable by opening one
	// plausible hand file (any two of this leaf's own candidate cards
	// make a valid "hand id" for this purpose -- we don't need it to be a
	// hand actually present in range_, just a file that should exist).
	bool probe_available() const {
		if (candidates.size() < 2) return false;
		int h1 = candidates[0], h2 = candidates[1];
		if (h1 > h2) std::swap(h1, h2);
		std::string path = split_dir_ + "/" + std::to_string(h1 * 52 + h2) + ".bin";
		int fd = open(path.c_str(), O_RDONLY);
		if (fd < 0) return false;
		close(fd);
		return true;
	}

	// Looks up river cluster ids for every (hand, candidate river card)
	// pair via a direct sparse pread() per candidate -- one file open/close
	// per unique hand, a few dozen tiny reads each, never a full-file load.
	void precompute(const std::vector<std::array<unsigned char, 2>>& hands, std::vector<std::vector<int>>& out) {
		out.assign(hands.size(), std::vector<int>(candidates.size(), -1));
		for (size_t hi = 0; hi < hands.size(); hi++) {
			unsigned char h[2] = { hands[hi][0], hands[hi][1] };
			if (h[0] == board_[0] || h[0] == board_[1] || h[0] == board_[2] || h[0] == board_[3] ||
				h[1] == board_[0] || h[1] == board_[1] || h[1] == board_[2] || h[1] == board_[3]) continue;
			int h1 = h[0], h2 = h[1];
			if (h1 > h2) std::swap(h1, h2);
			std::string path = split_dir_ + "/" + std::to_string(h1 * 52 + h2) + ".bin";
			int fd = open(path.c_str(), O_RDONLY);
			if (fd < 0) continue; // leave -1s; expected_showdown_sign() skips them
			for (size_t k = 0; k < candidates.size(); k++) {
				unsigned char c = candidates[k];
				if (c == h[0] || c == h[1]) continue;
				unsigned char comm[5] = { board_[0], board_[1], board_[2], board_[3], c };
				std::sort(comm, comm + 5);
				int compacted[5];
				for (int i = 0; i < 5; i++) compacted[i] = compact_index(comm[i], h1, h2);
				uint64_t R = lex_rank_of_combination(compacted, 5, 50);
				if (R >= (uint64_t)RIVER_COMMUNITY_TOTAL) continue;
				uint16_t val = 0;
				ssize_t got = pread(fd, &val, 2, VALUES_OFFSET + (long)R * 2);
				if (got == 2) out[hi][k] = (int)val;
			}
			close(fd);
		}
	}

	const Players_range& range_;
	std::string split_dir_;
	unsigned char board_[4];
	std::vector<unsigned char> candidates;
	std::vector<std::vector<int>> hero_clusters;
	std::vector<std::vector<int>> villain_clusters;
	bool available_ = false;
};

uint64_t RiverClusterLeafModel::C_TABLE[53][6] = {};
bool RiverClusterLeafModel::binomial_built_ = false;

// ---------------------------------------------------------------------------
// Range-vs-range vanilla vector-form CFR over a single flop betting round,
// built on Searchstate's real betting mechanics. This is new code; it does
// not read or write strategy_node/subgame_node (the existing blueprint /
// proprietary-adjacent raw-pointer tree types).
//
// cfr(node, reach, traverser) returns a vector, sized to `traverser`'s own
// range, of expected chip utility per traverser hand, integrated over the
// OTHER player's range as weighted by reach[other]. This is the standard
// "vector-form" vanilla CFR recursion (see e.g. Neller & Lanctot, "An
// Introduction to Counterfactual Regret Minimization", 2013, generalized
// here from a single hidden hand to full explicit ranges on both sides).
// ---------------------------------------------------------------------------
class FlopResolver {
public:
	FlopResolver(const Players_range& range, const TurnClusterLeafModel& leaf)
		: range_(range), leaf_(leaf) {
		N = (int)range_.hero.size();
		M = (int)range_.villain.size();
	}

	struct Node {
		Searchstate state;
		bool expanded = false;
		std::vector<unsigned char> actions;
		std::vector<std::vector<double>> regret;    // [own_hand_idx][action_idx]
		std::vector<std::vector<double>> strat_sum; // [own_hand_idx][action_idx], reach-weighted
		std::vector<std::unique_ptr<Node>> children;
	};

	std::unique_ptr<Node> root;

	void init_root(const Searchstate& s0) {
		root.reset(new Node());
		root->state = s0;
	}

	// Runs `iterations` full alternating-traverser CFR passes. Returns the
	// final average strategy for hero (player 0) hand index 0 at the root,
	// as a convenience for quick inspection; full strategies are available
	// via root->strat_sum after the call.
	void run(int iterations) {
		std::vector<double> reach0(N, 1.0), reach1(M, 1.0);
		for (int it = 0; it < iterations; it++) {
			std::vector<double> reach[2] = { reach0, reach1 };
			cfr(root.get(), reach, 0, true);
			std::vector<double> reach_b[2] = { reach0, reach1 };
			cfr(root.get(), reach_b, 1, true);
		}
	}

	// Average strategy at a node for a given own-hand index (normalized
	// strat_sum -- the standard CFR output policy).
	static void average_strategy(const Node* node, int hand_idx, std::vector<double>& out) {
		const auto& ss = node->strat_sum[hand_idx];
		double sum = 0.0;
		for (double v : ss) sum += v;
		out.assign(ss.size(), 0.0);
		if (sum > 1e-12) {
			for (size_t a = 0; a < ss.size(); a++) out[a] = ss[a] / sum;
		}
		else {
			for (size_t a = 0; a < ss.size(); a++) out[a] = 1.0 / ss.size();
		}
	}

	std::vector<double> cfr(Node* node, std::vector<double> reach[2], int traverser, bool update) {
		Searchstate& s = node->state;

		if (s.betting_stage == 5) return terminal_fold(node, reach, traverser);
		if (s.betting_stage >= 2) return terminal_leaf(node, reach, traverser);

		expand(node);
		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		int nA = (int)node->actions.size();

		std::vector<std::vector<double>> strategy(own_n);
		for (int h = 0; h < own_n; h++) regret_matching(node->regret[h], strategy[h]);

		std::vector<std::vector<double>> action_util(nA);
		for (int a = 0; a < nA; a++) {
			if (!node->children[a]) {
				node->children[a].reset(new Node());
				node->children[a]->state = s;
				node->children[a]->state.take_action(node->actions[a]);
			}
			std::vector<double> new_reach[2] = { reach[0], reach[1] };
			for (int h = 0; h < own_n; h++) new_reach[p][h] *= strategy[h][a];
			action_util[a] = cfr(node->children[a].get(), new_reach, traverser, update);
		}

		int out_n = (traverser == 0) ? N : M;
		std::vector<double> node_util(out_n, 0.0);
		if (p == traverser) {
			for (int h = 0; h < own_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += strategy[h][a] * action_util[a][h];
				node_util[h] = v;
				if (update) {
					for (int a = 0; a < nA; a++) {
						node->regret[h][a] += (action_util[a][h] - v);
						node->strat_sum[h][a] += reach[p][h] * strategy[h][a];
					}
				}
			}
		}
		else {
			// Opponent's decision node: the opponent's own strategy weighting
			// was already applied via new_reach[p] before recursing, so we
			// only need to sum the (already reach-weighted) child utilities.
			for (int h = 0; h < out_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += action_util[a][h];
				node_util[h] = v;
			}
		}
		return node_util;
	}

private:
	void expand(Node* node) {
		if (node->expanded) return;
		unsigned char buf[16];
		int n = node->state.legal_actions(buf);
		node->actions.assign(buf, buf + n);
		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		node->regret.assign(own_n, std::vector<double>(n, 0.0));
		node->strat_sum.assign(own_n, std::vector<double>(n, 0.0));
		node->children.resize(n);
		node->expanded = true;
	}

	std::vector<double> terminal_fold(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		int folder = s.table.players[0].active ? 1 : 0;
		int winner = 1 - folder;
		int other = 1 - traverser;
		double other_reach_sum = 0.0;
		for (double r : reach[other]) other_reach_sum += r;
		int out_n = (traverser == 0) ? (int)range_.hero.size() : (int)range_.villain.size();
		double v = (traverser == winner)
			? (double)(s.table.total_pot - s.table.players[traverser].n_bet_chips())
			: -(double)s.table.players[traverser].n_bet_chips();
		std::vector<double> util(out_n, v * other_reach_sum);
		return util;
	}

	std::vector<double> terminal_leaf(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		double pot = s.table.total_pot;
		int out_n = (traverser == 0) ? N : M;
		int other_n = (traverser == 0) ? M : N;
		std::vector<double> util(out_n, 0.0);
		for (int th = 0; th < out_n; th++) {
			double v = 0.0;
			for (int oh = 0; oh < other_n; oh++) {
				double r = reach[1 - traverser][oh];
				if (r == 0.0) continue;
				// Skip hero/villain hand pairs that share a physical card --
				// mutually impossible, must not contribute (see
				// hands_compatible()'s comment; this is a bug fix over the
				// initial version of this function, which only skipped
				// board-colliding candidates, not hero-vs-villain overlap).
				const auto& hero_hand = (traverser == 0) ? range_.hero[th] : range_.hero[oh];
				const auto& villain_hand = (traverser == 0) ? range_.villain[oh] : range_.villain[th];
				if (!hands_compatible(hero_hand, villain_hand)) continue;
				double sign = (traverser == 0)
					? leaf_.expected_showdown_sign(th, oh)
					: -leaf_.expected_showdown_sign(oh, th);
				v += r * sign * (pot / 2.0);
			}
			util[th] = v;
		}
		return util;
	}

	const Players_range& range_;
	const TurnClusterLeafModel& leaf_;
	int N, M;
};

// ---------------------------------------------------------------------------
// StreetChainResolver -- turn/river chaining.
//
// Extends the same idea as FlopResolver, but instead of stopping at a static
// leaf-value estimate when the flop betting round ends, it deals the actual
// turn card, resolves a real turn betting round, deals the actual river
// card, resolves a real river betting round, and only then evaluates an
// EXACT showdown via Engine::compute_winner()/find_strength() (the same
// exact 7-card hand-ranking table used elsewhere in this repo for training/
// simulation) -- no hand-cluster approximation is needed at all once the
// river is reached, since compute_winner() ranks the literal 7-card hand.
//
// Card deals are modeled as genuine chance nodes: every node whose known
// community-card count is behind what its betting_stage requires becomes a
// chance node that branches, with uniform probability, over every remaining
// card, and recurses. Any hero or villain hand whose hole cards collide with
// a just-dealt card has its reach-probability zeroed on that branch (that
// hand is simply impossible given that public card) -- and, symmetrically,
// hero/villain hands that are mutually incompatible (share a card with each
// other) are skipped at every showdown evaluation via hands_compatible().
//
// COST WARNING: this is combinatorially far more expensive than FlopResolver.
// A single flop-round leaf can fan out to ~49 turn-card branches, each of
// which can itself fan out to ~48 river-card branches -- so the tree this
// builds is easily hundreds to thousands of times larger than the flop-only
// tree for the same hero/villain range sizes. Use small explicit ranges
// (single-digit-to-low-tens of combos per side) unless you have deliberately
// budgeted for a much larger, much slower resolve. See BUILD_NOTES.md
// section 16 for measured runtimes/memory at a specific small scale and the
// scaling math for larger ones.
// ---------------------------------------------------------------------------
class StreetChainResolver {
public:
	StreetChainResolver(const Players_range& range, Engine* eng)
		: range_(range), engine_(eng) {
		N = (int)range_.hero.size();
		M = (int)range_.villain.size();
	}

	struct Node {
		Searchstate state;
		std::vector<unsigned char> board; // known community cards so far (3, 4, or 5)
		bool expanded = false;
		bool is_chance = false;
		std::vector<unsigned char> actions;      // decision node: legal actions
		std::vector<unsigned char> chance_cards; // chance node: remaining deck cards
		std::vector<std::vector<double>> regret;    // decision node only: [own_hand_idx][action_idx]
		std::vector<std::vector<double>> strat_sum; // decision node only, reach-weighted
		std::vector<std::unique_ptr<Node>> children;
	};

	std::unique_ptr<Node> root;

	void init_root(const Searchstate& s0, const unsigned char flop_board[3]) {
		root.reset(new Node());
		root->state = s0;
		root->board.assign(flop_board, flop_board + 3);
	}

	// Root reach vectors, with hands that collide with the fixed flop board
	// zeroed out from the start (they are simply impossible given that
	// board, and unlike later streets there is no chance node at the root
	// to naturally exclude them).
	void run(int iterations) {
		std::vector<double> reach0(N, 1.0), reach1(M, 1.0);
		for (int h = 0; h < N; h++)
			if (collides_with_board(range_.hero[h], root->board)) reach0[h] = 0.0;
		for (int h = 0; h < M; h++)
			if (collides_with_board(range_.villain[h], root->board)) reach1[h] = 0.0;
		for (int it = 0; it < iterations; it++) {
			std::vector<double> reach[2] = { reach0, reach1 };
			cfr(root.get(), reach, 0, true);
			std::vector<double> reach_b[2] = { reach0, reach1 };
			cfr(root.get(), reach_b, 1, true);
		}
	}

	static void average_strategy(const Node* node, int hand_idx, std::vector<double>& out) {
		const auto& ss = node->strat_sum[hand_idx];
		double sum = 0.0;
		for (double v : ss) sum += v;
		out.assign(ss.size(), 0.0);
		if (sum > 1e-12) {
			for (size_t a = 0; a < ss.size(); a++) out[a] = ss[a] / sum;
		}
		else {
			for (size_t a = 0; a < ss.size(); a++) out[a] = 1.0 / ss.size();
		}
	}

	// Total number of tree nodes built so far (decision + chance), for
	// reporting the combinatorial cost actually incurred by a given run.
	long long node_count() const { long long c = 0; count_nodes(root.get(), c); return c; }

	std::vector<double> cfr(Node* node, std::vector<double> reach[2], int traverser, bool update) {
		Searchstate& s = node->state;
		if (s.betting_stage == 5) return terminal_fold(node, reach, traverser);

		expand(node);
		if (node->is_chance) return chance_value(node, reach, traverser, update);
		if (s.betting_stage >= 4) return terminal_showdown(node, reach, traverser); // board.size()==5 guaranteed by expand()

		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		int nA = (int)node->actions.size();

		std::vector<std::vector<double>> strategy(own_n);
		for (int h = 0; h < own_n; h++) regret_matching(node->regret[h], strategy[h]);

		std::vector<std::vector<double>> action_util(nA);
		for (int a = 0; a < nA; a++) {
			if (!node->children[a]) {
				node->children[a].reset(new Node());
				node->children[a]->state = s;
				node->children[a]->state.take_action(node->actions[a]);
				node->children[a]->board = node->board; // dealing a card is not a betting action
			}
			std::vector<double> new_reach[2] = { reach[0], reach[1] };
			for (int h = 0; h < own_n; h++) new_reach[p][h] *= strategy[h][a];
			action_util[a] = cfr(node->children[a].get(), new_reach, traverser, update);
		}

		int out_n = (traverser == 0) ? N : M;
		std::vector<double> node_util(out_n, 0.0);
		if (p == traverser) {
			for (int h = 0; h < own_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += strategy[h][a] * action_util[a][h];
				node_util[h] = v;
				if (update) {
					for (int a = 0; a < nA; a++) {
						node->regret[h][a] += (action_util[a][h] - v);
						node->strat_sum[h][a] += reach[p][h] * strategy[h][a];
					}
				}
			}
		}
		else {
			for (int h = 0; h < out_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += action_util[a][h];
				node_util[h] = v;
			}
		}
		return node_util;
	}

private:
	static void count_nodes(const Node* n, long long& c) {
		if (!n) return;
		c++;
		for (auto& ch : n->children) count_nodes(ch.get(), c);
	}

	static bool collides_with_board(const std::array<unsigned char, 2>& hand, const std::vector<unsigned char>& board) {
		for (unsigned char b : board) if (hand[0] == b || hand[1] == b) return true;
		return false;
	}

	// How many community cards should be known once betting_stage reaches
	// this value (preflop=0 unused here, flop=1 -> 3, turn=2 -> 4, river=3
	// -> 5, shutdown/terminal=4 or 5 -> 5).
	static int cards_needed_for_stage(int stage) {
		if (stage <= 1) return 3;
		if (stage == 2) return 4;
		return 5;
	}

	void expand(Node* node) {
		if (node->expanded) return;
		node->expanded = true;
		int need = cards_needed_for_stage(node->state.betting_stage);
		if ((int)node->board.size() < need) {
			node->is_chance = true;
			bool used[52] = { false };
			for (unsigned char c : node->board) used[c] = true;
			for (int c = 0; c < 52; c++) if (!used[c]) node->chance_cards.push_back((unsigned char)c);
			node->children.resize(node->chance_cards.size());
			return;
		}
		if (node->state.betting_stage >= 4) return; // showdown terminal, nothing to expand
		unsigned char buf[16];
		int n = node->state.legal_actions(buf);
		// Reduced action abstraction for THIS resolver only (FlopResolver,
		// above, is untouched and keeps every native bet-size action).
		// Turn+river chance-node fanout (~49 x ~48 board runouts) makes the
		// full native raise-size ladder (0.5/1/2/4/8/10/20-pot options)
		// combinatorially infeasible to solve exactly here: a quick
		// full-action attempt did not finish 5 iterations in several
		// minutes. Collapsing every decision node to {fold, call/check,
		// all-in} keeps the per-street subtree small and shallow (depth
		// <=2, branching factor <=3) so the chained resolve is actually
		// tractable, at the honest cost of only exploring shove-or-fold
		// lines rather than intermediate bet sizes. See BUILD_NOTES.md
		// section 16 for the measured cost and scaling discussion.
		std::vector<unsigned char> reduced;
		for (int i = 0; i < n; i++)
			if (buf[i] == 'd' || buf[i] == 'l' || buf[i] == 'n') reduced.push_back(buf[i]);
		node->actions = reduced;
		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		node->regret.assign(own_n, std::vector<double>(node->actions.size(), 0.0));
		node->strat_sum.assign(own_n, std::vector<double>(node->actions.size(), 0.0));
		node->children.resize(node->actions.size());
	}

	std::vector<double> chance_value(Node* node, std::vector<double> reach[2], int traverser, bool update) {
		int out_n = (traverser == 0) ? N : M;
		std::vector<double> total(out_n, 0.0);
		int nC = (int)node->chance_cards.size();
		for (int ci = 0; ci < nC; ci++) {
			unsigned char c = node->chance_cards[ci];
			if (!node->children[ci]) {
				node->children[ci].reset(new Node());
				node->children[ci]->state = node->state;
				node->children[ci]->board = node->board;
				node->children[ci]->board.push_back(c);
			}
			std::vector<double> new_reach[2] = { reach[0], reach[1] };
			for (int h = 0; h < N; h++) if (range_.hero[h][0] == c || range_.hero[h][1] == c) new_reach[0][h] = 0.0;
			for (int h = 0; h < M; h++) if (range_.villain[h][0] == c || range_.villain[h][1] == c) new_reach[1][h] = 0.0;
			std::vector<double> child_util = cfr(node->children[ci].get(), new_reach, traverser, update);
			for (int h = 0; h < out_n; h++) total[h] += child_util[h] / nC;
		}
		return total;
	}

	std::vector<double> terminal_fold(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		int folder = s.table.players[0].active ? 1 : 0;
		int winner = 1 - folder;
		int other = 1 - traverser;
		double other_reach_sum = 0.0;
		for (double r : reach[other]) other_reach_sum += r;
		int out_n = (traverser == 0) ? N : M;
		double v = (traverser == winner)
			? (double)(s.table.total_pot - s.table.players[traverser].n_bet_chips())
			: -(double)s.table.players[traverser].n_bet_chips();
		return std::vector<double>(out_n, v * other_reach_sum);
	}

	std::vector<double> terminal_showdown(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		double pot = s.table.total_pot;
		double traverser_contrib = s.table.players[traverser].n_bet_chips();
		int out_n = (traverser == 0) ? N : M;
		int other_n = (traverser == 0) ? M : N;
		unsigned char comm[5] = { node->board[0], node->board[1], node->board[2], node->board[3], node->board[4] };
		std::vector<double> util(out_n, 0.0);
		for (int th = 0; th < out_n; th++) {
			// Guard against calling compute_winner()/find_strength() (which
			// THROWS on a lookup miss -- see poker/Engine.h) with a hand
			// that collides with the final board. reach[traverser][th] is
			// NOT a reliable proxy for this: it also gets scaled by the
			// traverser's OWN strategy probabilities as CFR trains (and can
			// legitimately be a small-but-nonzero or, in edge cases, exactly
			// zero fraction with no relation to physical card collisions),
			// so this must be checked directly against node->board instead.
			const auto& own_hand = (traverser == 0) ? range_.hero[th] : range_.villain[th];
			if (collides_with_board(own_hand, node->board)) { util[th] = 0.0; continue; }
			double v = 0.0;
			for (int oh = 0; oh < other_n; oh++) {
				double r = reach[1 - traverser][oh];
				if (r == 0.0) continue;
				const auto& hero_hand = (traverser == 0) ? range_.hero[th] : range_.hero[oh];
				const auto& villain_hand = (traverser == 0) ? range_.villain[oh] : range_.villain[th];
				const auto& other_hand = (traverser == 0) ? villain_hand : hero_hand;
				if (collides_with_board(other_hand, node->board)) continue;
				if (!hands_compatible(hero_hand, villain_hand)) continue;
				unsigned char p0[2] = { hero_hand[0], hero_hand[1] };
				unsigned char p1[2] = { villain_hand[0], villain_hand[1] };
				unsigned char w = engine_->compute_winner(p0, p1, comm);
				double val;
				if (w == 255) val = pot / 2.0 - traverser_contrib;
				else if ((int)w == traverser) val = pot - traverser_contrib;
				else val = -traverser_contrib;
				v += r * val;
			}
			util[th] = v;
		}
		return util;
	}

	const Players_range& range_;
	Engine* engine_;
	int N, M;
};

// ---------------------------------------------------------------------------
// LiveResolver -- a fast, reduced-action resolver meant for actual interactive
// play (see tools/dh_native_ai.cpp), not for the flop/turn/river-chaining
// accuracy studies above. It always restricts every decision node to just
// {fold, call/check, all-in} (see FlopResolver/StreetChainResolver's fuller,
// slower native action ladder for a non-live-play alternative) so that a
// live decision reliably returns in well under a second, and it can be
// pointed at any single street via `Mode`:
//
//   Mode::FLOP  -- root board has 3 cards, betting_stage==1. Terminal is the
//                  same approximate turn-cluster-comparison leaf model used
//                  by FlopResolver (TurnClusterLeafModel) the instant the
//                  flop betting round ends -- no turn/river cards are
//                  actually dealt in this mode.
//   Mode::TURN  -- root board has 4 cards, betting_stage==2. Turn betting is
//                  resolved for real; once it closes, ONE real river card is
//                  dealt via a genuine chance node (~48 branches) and the
//                  hand is then valued by an EXACT showdown -- but no river
//                  BETTING round is modeled (this assumes the river gets
//                  checked down, a deliberate, documented approximation
//                  adopted purely so a turn decision resolves fast enough
//                  for live play; see StreetChainResolver, above, for the
//                  slower-but-exact alternative that models real river
//                  betting too).
//   Mode::RIVER -- root board already has all 5 cards, betting_stage==3.
//                  River betting is resolved for real (no cards left to
//                  deal), terminating in an EXACT showdown -- this mode is
//                  exact, with no approximation beyond the reduced action
//                  set.
//
// Like every resolver in this file, this performs UNSAFE (fixed-range)
// subgame resolving in the classical sense (no true opponent-modeling
// search over hero's own strategy is performed -- only a range-vs-range
// vanilla CFR resolve against whatever range the caller supplies). That
// caller-supplied range is no longer necessarily a flat/uniform sample,
// though: dh_native_ai.cpp tracks a persistent, full (non-fixed-size)
// opponent-range belief across the hand (LiveGame::villain_range),
// seeded from the real trained preflop blueprint and narrowed after each
// observed opponent action via this same resolver's own strat_sum/
// average_strategy() output (see run()'s external_reach parameters,
// below, and dh_native_ai.cpp's narrow_villain_range_postflop()) -- this
// header only provides the mechanism (an externally-seedable initial
// reach), not the belief-tracking policy itself, which lives entirely in
// the caller. hero's range is typically just the single real hand held by
// whichever side is "me" in the live game, which keeps N small and the
// resolve fast regardless of mode.
// ---------------------------------------------------------------------------
class LiveResolver {
public:
	enum class Mode { FLOP, TURN, RIVER };

	// `extended_actions`, when true, adds ONE extra genuine tree branch to
	// the otherwise-reduced fold/call/allin action set: a canonical
	// pot-sized raise (native action byte 2 -- see State.h's take_action(),
	// which already implements it exactly like every other pot-fraction
	// raise; nothing new is added to the engine itself, only to this
	// resolver's own action filter). Default is false, so hero's own live
	// decisions (resolve_decision() in dh_native_ai.cpp) are completely
	// unaffected -- this flag exists so narrow_villain_range_postflop() can
	// build a SEPARATE, purpose-built resolver instance that gives an
	// observed non-all-in opponent raise a real node to narrow against,
	// without changing what hero itself is able to do. See BUILD_NOTES.md
	// for the full writeup (why one bucket, why not the full ladder, and
	// the measured cost of the 4th action).
	// `river_leaf`, when non-null AND river_leaf->available(), gives TURN
	// mode (see BUILD_NOTES.md section 34) the same kind of cheap terminal
	// shortcut FLOP mode already has via `leaf`: once TURN betting closes,
	// instead of dealing a real river chance node and resolving an exact
	// showdown for every one of its ~48 branches, this estimates the leaf
	// value directly from precomputed river-cluster ids read from the
	// per-hole-hand split files. When null (or unavailable), TURN mode
	// behaves exactly as before this feature was added -- a real chance
	// node + exact showdown -- so this is purely additive/opt-in.
	//
	// `full_ladder`, when true, keeps EVERY legal action State.h's
	// legal_actions() returns at the OPENING decision of a betting round
	// (cur_round_action_num==0, i.e. nobody has acted yet this street) --
	// the same native pot-fraction raise sizes (0.5/1/2/4/8/10/20 pot,
	// subject to the same per-round/per-raise caps used everywhere else
	// in this codebase, including the trained blueprint) the reduced
	// fold/call/allin-only set above was collapsing away, instead of
	// inventing new sizes. Nodes reached AFTER that opening action (i.e.
	// facing a bet/raise) still fall back to the reduced set, plus the
	// same single extra "1x pot" size extended_actions_ already offers
	// (the native abstraction itself never offers more than that one
	// extra size once facing a bet anyway). This restriction is
	// deliberate and measured, not a simplification for its own sake:
	// keeping the FULL ladder at every depth (including deep in a
	// reraise war) was benchmarked at 6-75x slower per iteration and
	// pushed convergence out to 17-60+ seconds -- unusable for a live
	// decision -- because the opening node's up to 6-way branching
	// factor compounds through every subtree beneath it. Restricting
	// full granularity to just the opening action keeps convergence
	// times in the same ballpark as the reduced action set while still
	// letting hero pick a real, differentiated bet size instead of only
	// fold/check/allin (see BUILD_NOTES.md section 37 for the numbers).
	// This is only safe to combine with modes that never expand a
	// further chance node inside THIS resolver's own tree (FLOP --
	// always terminates at `leaf` the instant flop betting closes; TURN
	// -- terminates at `river_leaf` the instant turn betting closes, but
	// ONLY when river_leaf is actually active; RIVER -- the last street,
	// nothing further to deal). TURN mode WITHOUT an active river_leaf
	// still deals a real, expensive river chance node per iteration, and
	// full_ladder combined with that case reproduces the original "did
	// not finish 5 iterations in several minutes" combinatorial blowup
	// this file's `expand()` used to warn about -- so callers must gate
	// full_ladder on river_leaf availability themselves for TURN mode
	// (dh_native_ai.cpp does this).
	LiveResolver(const Players_range& range, Engine* eng, const TurnClusterLeafModel* leaf, Mode mode,
		bool extended_actions = false, const RiverClusterLeafModel* river_leaf = nullptr,
		bool full_ladder = false)
		: range_(range), engine_(eng), leaf_(leaf), river_leaf_(river_leaf), mode_(mode),
		  extended_actions_(extended_actions), full_ladder_(full_ladder) {
		N = (int)range_.hero.size();
		M = (int)range_.villain.size();
#ifdef _OPENMP
		// Same default TexasSolver uses (src/solver/PCfrSolver.cpp): use
		// every logical core visible to this process. Explicit rather than
		// relying on OpenMP's own default so behavior doesn't silently
		// change if some other library/tool has set OMP_NUM_THREADS in the
		// environment this process inherits.
		omp_set_num_threads(omp_get_num_procs());
#endif
	}

	struct Node {
		Searchstate state;
		std::vector<unsigned char> board;
		bool expanded = false;
		bool is_chance = false;
		std::vector<unsigned char> actions;
		std::vector<unsigned char> chance_cards;
		std::vector<std::vector<double>> regret;
		std::vector<std::vector<double>> strat_sum;
		std::vector<std::unique_ptr<Node>> children;

		// Lazily-built, iteration-invariant terminal-value caches (BUILD_NOTES.md
		// section 46). A terminal node's board is FIXED for its entire lifetime
		// (children/nodes are created once and reused across every subsequent
		// CFR iteration -- see the "if (!node->children[a])" pattern used
		// throughout expand()/chance_value()), so any value that depends ONLY on
		// (this node's fixed board, a hero combo, a villain combo) -- never on
		// reach weights or iteration number -- only needs computing ONCE, ever,
		// no matter how many of the (up to 20000) CFR iterations revisit this
		// same node. Before this cache existed, terminal_showdown/terminal_leaf/
		// terminal_river_leaf recomputed these from scratch on every single
		// iteration.
		bool strength_cache_ready = false;
		std::vector<int> hero_strength_cache;    // terminal_showdown: Maxstrength() per hero combo vs. this node's board
		std::vector<int> villain_strength_cache; // terminal_showdown: Maxstrength() per villain combo vs. this node's board
		bool leaf_sign_cache_ready = false;
		std::vector<std::vector<float>> leaf_sign_cache;        // terminal_leaf: expected_showdown_sign(hi, vi) per (hero, villain) combo pair
		bool river_leaf_sign_cache_ready = false;
		std::vector<std::vector<float>> river_leaf_sign_cache;  // terminal_river_leaf: same, via river_leaf_ instead of leaf_
	};

	std::unique_ptr<Node> root;

	void init_root(const Searchstate& s0, const std::vector<unsigned char>& board0) {
		root.reset(new Node());
		root->state = s0;
		root->board = board0;
	}

	// `external_reach0`/`external_reach1`, when supplied and correctly
	// sized, seed the hero/villain side's initial per-hand reach weights
	// (e.g. a caller-tracked opponent-range belief) instead of the default
	// flat 1.0-per-hand assumption. This does not change the resolver's
	// mechanics at all -- reach weights already flow through cfr() exactly
	// like this for every iteration -- it only changes what they start as.
	// See dh_native_ai.cpp's LiveGame::villain_range for the caller that
	// uses this to narrow a persistent, full (non-fixed-size) opponent
	// range belief street-by-street, rather than treating every hand in
	// the supplied range as equally likely.
	void run(int iterations,
		const std::vector<double>* external_reach0 = nullptr,
		const std::vector<double>* external_reach1 = nullptr) {
		std::vector<double> reach0(N, 1.0), reach1(M, 1.0);
		if (external_reach0 && (int)external_reach0->size() == N) reach0 = *external_reach0;
		if (external_reach1 && (int)external_reach1->size() == M) reach1 = *external_reach1;
		for (int h = 0; h < N; h++)
			if (collides_with_board(range_.hero[h], root->board)) reach0[h] = 0.0;
		for (int h = 0; h < M; h++)
			if (collides_with_board(range_.villain[h], root->board)) reach1[h] = 0.0;
		for (int it = 0; it < iterations; it++) {
			std::vector<double> reach[2] = { reach0, reach1 };
			cfr(root.get(), reach, 0, true, 0);
			std::vector<double> reach_b[2] = { reach0, reach1 };
			cfr(root.get(), reach_b, 1, true, 0);
		}
	}

	static void average_strategy(const Node* node, int hand_idx, std::vector<double>& out) {
		const auto& ss = node->strat_sum[hand_idx];
		double sum = 0.0;
		for (double v : ss) sum += v;
		out.assign(ss.size(), 0.0);
		if (sum > 1e-12) {
			for (size_t a = 0; a < ss.size(); a++) out[a] = ss[a] / sum;
		}
		else {
			for (size_t a = 0; a < ss.size(); a++) out[a] = 1.0 / ss.size();
		}
	}

	// Depth-limited action/chance-card fan-out parallelism (see
	// BUILD_NOTES.md for the writeup). This is a PURE performance change,
	// not an algorithm change: every action's (or chance card's) subtree
	// is already, by construction, a fully independent Node object --
	// node->children[a] for different `a` never alias each other, they
	// are only combined (into node_util/total) AFTER every recursive call
	// has returned, in the exact same left-to-right order the original
	// serial loop used. Running those independent recursive calls on
	// separate threads therefore produces bit-identical results to the
	// serial version; it does not touch the alternating-update CFR
	// ordering (cfr(...,traverser=0,...) still fully completes, in
	// program order, before cfr(...,traverser=1,...) begins -- see run()
	// -- so the solver semantics are completely unchanged.
	// Only shallow depths are parallelized (kParallelDepthCutoff), since
	// thread-dispatch overhead would dominate for the many small nodes
	// deep in the tree. With OpenMP (see parallel_map() below) this also
	// falls out naturally: OpenMP disables NESTED parallel regions by
	// default (confirmed on this host -- an inner "#pragma omp parallel
	// for" issued from inside an already-parallel outer region runs with
	// a team size of 1, i.e. serially, with zero extra overhead), so once
	// one level of the recursion has fanned out across all cores, any
	// deeper opportunity naturally collapses back to serial execution
	// with no oversubscription. kParallelDepthCutoff is kept anyway as a
	// belt-and-suspenders bound and to document intent.
	static constexpr int kParallelDepthCutoff = 2;

	// Even a lightweight OpenMP fork-join still costs something (thread
	// wakeup + an implicit barrier at the end of the "#pragma omp parallel
	// for" region), so it only pays for itself when there is enough
	// independent work per branch to amortize that cost. Measured
	// directly (see BUILD_NOTES.md): the ~44-48-branch TURN river chance
	// node is a rich parallelization target, while the small (2-4-action)
	// fold/call/raise/allin decision nodes that dominate FLOP/RIVER mode
	// have per-branch work small enough that gating still matters.
	// Gating on branch count (not mode) keeps this general: it lets a
	// wide node (a real chance node, or a full-ladder opening action with
	// several native pot-fraction sizes) parallelize, while narrow nodes
	// (the common facing-a-bet fold/call/allin case) fall back to the
	// original serial loop with zero added overhead.
	//
	// This gate is NOT just an overhead optimization under OpenMP -- it is
	// load-bearing for correctness-of-performance given OpenMP disables
	// NESTED parallel regions by default (verified on this host). Only
	// ONE level of the recursion (whichever hits the gate first, within
	// kParallelDepthCutoff) actually fans out across cores; every
	// "#pragma omp parallel for" issued from inside an already-active
	// parallel region collapses to a team of size 1. Measured directly:
	// lowering this gate to 2 (so a depth-0 node with only 3-4 actions
	// grabs the single available parallel slot) made TURN mode ~2x
	// SLOWER (it never reaches its own much wider depth-1 river chance
	// node, which is now nested and serial), even though it slightly
	// helped RIVER mode (~14%, whose tree has no chance node to steal
	// the slot from at all). Keeping the gate high enough that only a
	// genuine chance node satisfies it reserves the one available
	// parallel opportunity for whichever node benefits from it most.
	static constexpr int kMinParallelBranchCount = 8;

	// Runs `n` independent tasks (each producing a std::vector<double>),
	// either via a "#pragma omp parallel for" (the same OpenMP-based
	// persistent-thread-pool mechanism $HOME/src/TexasSolver uses --
	// src/solver/PCfrSolver.cpp / slice_cfr.cpp -- rather than spawning a
	// fresh OS thread per call the way std::async did) when `depth` is
	// shallow enough and `n` is large enough to be worth it, or serially
	// otherwise -- and always serially if built without OpenMP support
	// (_OPENMP undefined). `make_task(i)` must return a zero-argument
	// callable producing the i-th result; results[i] is written only by
	// the iteration that owns index i, so concurrent writes never alias.
	template <class MakeTask>
	std::vector<std::vector<double>> parallel_map(int n, int depth, MakeTask&& make_task) {
		std::vector<std::vector<double>> results(n);
#ifdef _OPENMP
		if (depth < kParallelDepthCutoff && n >= kMinParallelBranchCount) {
			// A cfr() call can, in principle, throw (e.g. a corrupt/short
			// cluster-lookup read surfacing as std::exception deeper in
			// the call chain). An exception escaping an OpenMP parallel
			// region uncaught is undefined behavior (typically an
			// immediate std::terminate), so each iteration catches and
			// stashes the first exception it sees; it is rethrown, from
			// ordinary serial context, once the parallel region (and its
			// implicit barrier) has fully finished -- preserving the same
			// "exception propagates to the caller" behavior the serial
			// loop always had.
			std::exception_ptr first_error;
			#pragma omp parallel for schedule(dynamic)
			for (int i = 0; i < n; i++) {
				try {
					results[i] = make_task(i)();
				}
				catch (...) {
					#pragma omp critical
					{
						if (!first_error) first_error = std::current_exception();
					}
				}
			}
			if (first_error) std::rethrow_exception(first_error);
			return results;
		}
#endif
		for (int i = 0; i < n; i++) results[i] = make_task(i)();
		return results;
	}

	std::vector<double> cfr(Node* node, std::vector<double> reach[2], int traverser, bool update, int depth) {
		Searchstate& s = node->state;
		if (s.betting_stage == 5) return terminal_fold(node, reach, traverser);
		if (mode_ == Mode::FLOP && s.betting_stage >= 2) return terminal_leaf(node, reach, traverser);
		if (mode_ == Mode::TURN && river_leaf_ && river_leaf_->available() && s.betting_stage >= 3)
			return terminal_river_leaf(node, reach, traverser);
		if (mode_ == Mode::TURN && (int)node->board.size() >= 5) return terminal_showdown(node, reach, traverser);

		expand(node);
		if (node->is_chance) return chance_value(node, reach, traverser, update, depth);
		if (s.betting_stage >= 4) return terminal_showdown(node, reach, traverser);

		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		int nA = (int)node->actions.size();

		std::vector<std::vector<double>> strategy(own_n);
		for (int h = 0; h < own_n; h++) regret_matching(node->regret[h], strategy[h]);

		// Pre-create all children BEFORE any parallel dispatch: creating a
		// std::unique_ptr<Node> at DISTINCT vector indices concurrently is
		// safe (distinct objects, no reallocation since node->children was
		// already sized by expand()), but doing it here, serially, up
		// front keeps the parallel section itself free of any writes to
		// shared parent state -- only per-branch-local new_reach and the
		// recursive call itself happen inside each task.
		for (int a = 0; a < nA; a++) {
			if (!node->children[a]) {
				node->children[a].reset(new Node());
				node->children[a]->state = s;
				node->children[a]->state.take_action(node->actions[a]);
				node->children[a]->board = node->board;
			}
		}
		auto action_util = parallel_map(nA, depth, [&](int a) {
			return [this, node, &reach, &strategy, own_n, p, traverser, update, depth, a]() {
				std::vector<double> new_reach[2] = { reach[0], reach[1] };
				for (int h = 0; h < own_n; h++) new_reach[p][h] *= strategy[h][a];
				return cfr(node->children[a].get(), new_reach, traverser, update, depth + 1);
			};
		});

		int out_n = (traverser == 0) ? N : M;
		std::vector<double> node_util(out_n, 0.0);
		if (p == traverser) {
			for (int h = 0; h < own_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += strategy[h][a] * action_util[a][h];
				node_util[h] = v;
				if (update) {
					for (int a = 0; a < nA; a++) {
						node->regret[h][a] += (action_util[a][h] - v);
						node->strat_sum[h][a] += reach[p][h] * strategy[h][a];
					}
				}
			}
		}
		else {
			for (int h = 0; h < out_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += action_util[a][h];
				node_util[h] = v;
			}
		}
		return node_util;
	}

	// ---------------------------------------------------------------------
	// Exploitability measurement: how far the AVERAGE strategy accumulated
	// by run() so far is from a Nash equilibrium of this resolved subgame.
	// Added to let live iteration budgets be chosen from a measured
	// convergence target (e.g. "keep iterating until exploitability drops
	// below 1% of the pot") instead of an arbitrary fixed iteration count.
	// See tools/test_resolver_exploitability.cpp for the tool that uses
	// this, and BUILD_NOTES.md for the measured per-street numbers.
	//
	// best_response(node, reach, traverser) computes, for each of
	// `traverser`'s own hands, the value achievable by best-responding
	// (choosing the single best action at every one of traverser's own
	// decision nodes) against the OTHER player's CURRENT AVERAGE strategy
	// (via the existing static average_strategy(), not the per-iteration
	// regret-matching strategy cfr() itself uses). It deliberately mirrors
	// cfr()'s control flow exactly (same terminal-node shortcuts, same
	// mode-specific FLOP/TURN/RIVER handling) and reuses cfr()'s own
	// terminal_fold/terminal_leaf/terminal_showdown unchanged: those three
	// functions already only depend on the OTHER player's reach weights,
	// never on any strategy of `traverser`, so they are exactly correct
	// for a best-response computation with no changes at all. The only
	// node type needing new handling is a genuine decision node: when the
	// acting player IS traverser, take the per-hand MAX over actions (a
	// best response is always a pure choice at every real decision point,
	// not cfr()'s current-iteration mixed strategy); when the acting
	// player is the OTHER player, weight by their AVERAGE strategy instead
	// of a single iteration's regret-matching strategy.
	std::vector<double> best_response(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		if (s.betting_stage == 5) return terminal_fold(node, reach, traverser);
		if (mode_ == Mode::FLOP && s.betting_stage >= 2) return terminal_leaf(node, reach, traverser);
		if (mode_ == Mode::TURN && river_leaf_ && river_leaf_->available() && s.betting_stage >= 3)
			return terminal_river_leaf(node, reach, traverser);
		if (mode_ == Mode::TURN && (int)node->board.size() >= 5) return terminal_showdown(node, reach, traverser);

		expand(node);
		if (node->is_chance) return chance_value_br(node, reach, traverser);
		if (s.betting_stage >= 4) return terminal_showdown(node, reach, traverser);

		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		int nA = (int)node->actions.size();

		std::vector<std::vector<double>> avg_strategy;
		if (p != traverser) {
			avg_strategy.resize(own_n);
			for (int h = 0; h < own_n; h++) average_strategy(node, h, avg_strategy[h]);
		}

		std::vector<std::vector<double>> action_util(nA);
		for (int a = 0; a < nA; a++) {
			if (!node->children[a]) {
				node->children[a].reset(new Node());
				node->children[a]->state = s;
				node->children[a]->state.take_action(node->actions[a]);
				node->children[a]->board = node->board;
			}
			std::vector<double> new_reach[2] = { reach[0], reach[1] };
			if (p != traverser) {
				for (int h = 0; h < own_n; h++) new_reach[p][h] *= avg_strategy[h][a];
			}
			// When p == traverser, reach is left untouched: a best
			// response's own hand-conditional value doesn't depend on any
			// mixed strategy traverser might assign to their own actions
			// -- we're computing the max, not an expectation over a
			// traverser-side mixture.
			action_util[a] = best_response(node->children[a].get(), new_reach, traverser);
		}

		int out_n = (traverser == 0) ? N : M;
		std::vector<double> node_util(out_n, 0.0);
		if (p == traverser) {
			for (int h = 0; h < own_n; h++) {
				double best = action_util[0][h];
				for (int a = 1; a < nA; a++) if (action_util[a][h] > best) best = action_util[a][h];
				node_util[h] = best;
			}
		}
		else {
			for (int h = 0; h < out_n; h++) {
				double v = 0.0;
				for (int a = 0; a < nA; a++) v += action_util[a][h];
				node_util[h] = v;
			}
		}
		return node_util;
	}

	// Full exploitability of the average strategy accumulated so far, in
	// raw chips: BR0 (player 0's best-response value against player 1's
	// current average strategy, weighted by `prior0` -- a probability
	// distribution over player 0's own hand range) plus BR1 (the mirror
	// for player 1, weighted by `prior1`). In this zero-sum resolve (no
	// rake, chips only change hands) this equals the textbook
	// exploitability formula (BR0 - v0) + (BR1 - v1), where v0/v1 are the
	// average-strategy-vs-average-strategy values for each player: because
	// v0 + v1 = 0 exactly in a zero-sum game, those two terms cancel and
	// BR0 + BR1 IS the exploitability directly, with no separate
	// average-vs-average value computation needed. Pass nullptr for
	// `prior0`/`prior1` to use a uniform prior over each side's
	// non-board-colliding hands (the right choice for a general
	// convergence measurement); pass a real tracked-range weight vector
	// (e.g. dh_native_ai.cpp's LiveGame::villain_range weights) to measure
	// exploitability of THIS specific live decision's actual belief state
	// instead. Supplied priors are defensively re-masked for board
	// collisions and renormalized here -- callers are not required to have
	// already done so (board cards can outpace a range snapshot taken
	// earlier in the street).
	double exploitability(const std::vector<double>* prior0 = nullptr, const std::vector<double>* prior1 = nullptr) {
		std::vector<double> reach0(N, 1.0), reach1(M, 1.0);
		for (int h = 0; h < N; h++) if (collides_with_board(range_.hero[h], root->board)) reach0[h] = 0.0;
		for (int h = 0; h < M; h++) if (collides_with_board(range_.villain[h], root->board)) reach1[h] = 0.0;

		std::vector<double> prior0_v = normalize_prior(prior0, reach0, N);
		std::vector<double> prior1_v = normalize_prior(prior1, reach1, M);

		// IMPORTANT: the terminal value functions (terminal_fold/leaf/
		// showdown, reused unmodified from cfr()) sum the traverser's
		// payoff directly over reach[1-traverser] with no normalization
		// of their own -- during ordinary CFR training that's fine
		// (regret-matching's action ratios are invariant to a uniform
		// rescaling of the opponent's reach), but here we need an actual
		// chip-denominated expected value, so the OPPONENT'S reach must
		// already be a normalized probability distribution (sums to 1),
		// not the raw board-collision 0/1 mask -- otherwise the returned
		// value scales up by the opponent's range size (off by ~1000x for
		// a full ~1000-combo range) instead of being a proper average.
		std::vector<double> reach_for_br0[2] = { reach0, prior1_v };
		std::vector<double> br0_per_hand = best_response(root.get(), reach_for_br0, 0);
		double br0 = 0.0;
		for (int h = 0; h < N; h++) br0 += prior0_v[h] * br0_per_hand[h];

		std::vector<double> reach_for_br1[2] = { prior0_v, reach1 };
		std::vector<double> br1_per_hand = best_response(root.get(), reach_for_br1, 1);
		double br1 = 0.0;
		for (int h = 0; h < M; h++) br1 += prior1_v[h] * br1_per_hand[h];

		return br0 + br1;
	}

private:
	static std::vector<double> uniform_prior(const std::vector<double>& reach_mask) {
		int valid = 0;
		for (double r : reach_mask) if (r > 0.0) valid++;
		std::vector<double> out(reach_mask.size(), 0.0);
		if (valid == 0) return out;
		double w = 1.0 / valid;
		for (size_t i = 0; i < reach_mask.size(); i++) if (reach_mask[i] > 0.0) out[i] = w;
		return out;
	}

	// If `prior` is supplied, mask it to zero out any board-colliding hand
	// (index-aligned with `reach_mask`, whose sign already encodes that)
	// and renormalize to sum to 1; otherwise fall back to a uniform prior
	// over the non-colliding hands. `expected_n` guards against a
	// caller-supplied prior of the wrong size (e.g. stale from a previous
	// street with a different tracked-range length) by falling back to
	// uniform in that case too, rather than indexing out of bounds.
	static std::vector<double> normalize_prior(const std::vector<double>* prior,
		const std::vector<double>& reach_mask, int expected_n) {
		if (!prior || (int)prior->size() != expected_n) return uniform_prior(reach_mask);
		std::vector<double> out(expected_n, 0.0);
		double sum = 0.0;
		for (int i = 0; i < expected_n; i++) {
			double v = (reach_mask[i] > 0.0) ? (*prior)[i] : 0.0;
			out[i] = v;
			sum += v;
		}
		if (!(sum > 1e-12)) return uniform_prior(reach_mask);
		for (int i = 0; i < expected_n; i++) out[i] /= sum;
		return out;
	}

	// Mirrors chance_value() exactly, but recurses into best_response()
	// instead of cfr() -- chance nodes have no strategy of their own (a
	// dealt card isn't chosen by either player), so the only change needed
	// is which function the recursion calls.
	std::vector<double> chance_value_br(Node* node, std::vector<double> reach[2], int traverser) {
		int out_n = (traverser == 0) ? N : M;
		std::vector<double> total(out_n, 0.0);
		int nC = (int)node->chance_cards.size();
		for (int ci = 0; ci < nC; ci++) {
			unsigned char c = node->chance_cards[ci];
			if (!node->children[ci]) {
				node->children[ci].reset(new Node());
				node->children[ci]->state = node->state;
				node->children[ci]->board = node->board;
				node->children[ci]->board.push_back(c);
			}
			std::vector<double> new_reach[2] = { reach[0], reach[1] };
			for (int h = 0; h < N; h++) if (range_.hero[h][0] == c || range_.hero[h][1] == c) new_reach[0][h] = 0.0;
			for (int h = 0; h < M; h++) if (range_.villain[h][0] == c || range_.villain[h][1] == c) new_reach[1][h] = 0.0;
			std::vector<double> child_util = best_response(node->children[ci].get(), new_reach, traverser);
			for (int h = 0; h < out_n; h++) total[h] += child_util[h] / nC;
		}
		return total;
	}

	static bool collides_with_board(const std::array<unsigned char, 2>& hand, const std::vector<unsigned char>& board) {
		for (unsigned char b : board) if (hand[0] == b || hand[1] == b) return true;
		return false;
	}

	static int cards_needed_for_stage(int stage) {
		if (stage <= 1) return 3;
		if (stage == 2) return 4;
		return 5;
	}

	void expand(Node* node) {
		if (node->expanded) return;
		node->expanded = true;
		int need = cards_needed_for_stage(node->state.betting_stage);
		if ((int)node->board.size() < need) {
			node->is_chance = true;
			bool used[52] = { false };
			for (unsigned char c : node->board) used[c] = true;
			for (int c = 0; c < 52; c++) if (!used[c]) node->chance_cards.push_back((unsigned char)c);
			node->children.resize(node->chance_cards.size());
			return;
		}
		if (node->state.betting_stage >= 4) return;
		unsigned char buf[16];
		int n = node->state.legal_actions(buf);
		if (full_ladder_ && node->state.cur_round_action_num == 0) {
			// Keep EVERY legal action -- the same native pot-fraction raise
			// ladder (0.5/1/2/4/8/10/20 pot, per State.h's legal_actions())
			// the trained blueprint and every other part of this codebase
			// already uses, not a new invented size (BUILD_NOTES.md section
			// 37). Only safe for modes that don't expand a further chance
			// node inside this tree -- see the constructor comment; callers
			// are responsible for that gating.
			//
			// Restricted to cur_round_action_num==0 (the FIRST action of
			// the betting round, i.e. nobody has acted yet this street):
			// measured (BUILD_NOTES.md section 37) that keeping the full
			// ladder at EVERY node, including facing-a-raise nodes deeper
			// in a reraise war, made FLOP/RIVER/TURN(leaf) 6-75x slower
			// per iteration and pushed convergence out to 17-60+ seconds
			// -- unusable for a live per-decision response. Since
			// State.h's own legal_actions() already collapses raise
			// choices down to at most one extra pot-size once facing a
			// reraise (cur_round_action_num in [2,4)) and none at all
			// beyond that (>=4), the real combinatorial cost was almost
			// entirely the opening node's up-to-6-way branching factor
			// compounding through the reduced-but-still-real subtrees
			// beneath each of those branches. Offering full granularity
			// only at the single opening decision (falling back to the
			// existing fold/call/allin -- or extended_actions_'s +1x-pot
			// -- set for every node after that) keeps the dominant new
			// information (which OPENING size hero used) while avoiding
			// that compounding cost.
			node->actions.assign(buf, buf + n);
		} else {
			std::vector<unsigned char> reduced;
			// Byte 2 ("1x pot" raise, per State.h's take_action()) is included
			// when extended_actions_ is set, OR when full_ladder_ is set and
			// we're past the opening node (facing-a-raise/reraise): the
			// native abstraction itself only ever offers at most this one
			// extra pot-fraction-raise size once facing a bet via the
			// cur_round_action_num-gated branch (State.h's
			// cur_round_action_num in [2,4) gate), so keeping it here is a
			// single extra branch, not a combinatorial blowup -- unlike
			// keeping the full 6-way opening ladder at every depth, which
			// was measured to be 6-75x slower per iteration (BUILD_NOTES.md
			// section 37).
			//
			// Byte 1 ("0.5x pot" raise) is ADDITIONALLY included when
			// extended_actions_ is set (BUILD_NOTES.md section 51): this is
			// also a real, already-existing native action -- State.h's
			// legal_actions() offers it any time n_raises<2 for the whole
			// street, independent of cur_round_action_num, so it is not an
			// invented size either. This widens narrow_villain_range_
			// postflop()'s single "canonical 1x pot" narrowing bucket to two
			// buckets (0.5x and 1x pot) after measuring that Slumbot's real
			// postflop raises are heavily concentrated BELOW 1x pot (median
			// ~0.67x pot, 85% under 0.9x pot, see BUILD_NOTES.md section 51)
			// -- collapsing all of those onto a single "as if pot-sized"
			// bucket was a measurable range-narrowing distortion on every
			// hand with a villain postflop raise. Deliberately NOT added to
			// full_ladder_'s post-opening-node branch: full_ladder_ is only
			// used by resolve_decision() (hero's own live decision), which
			// intentionally keeps the ORIGINAL reduced set there (this
			// change is scoped to narrow_villain_range_postflop()'s belief
			// update only, exactly like extended_actions_ already was).
			for (int i = 0; i < n; i++)
				if (buf[i] == 'd' || buf[i] == 'l' || buf[i] == 'n' ||
					(extended_actions_ && buf[i] == 1) ||
					((extended_actions_ || full_ladder_) && buf[i] == 2)) reduced.push_back(buf[i]);
			node->actions = reduced;
		}
		int p = node->state.player_i_index;
		int own_n = (p == 0) ? N : M;
		node->regret.assign(own_n, std::vector<double>(node->actions.size(), 0.0));
		node->strat_sum.assign(own_n, std::vector<double>(node->actions.size(), 0.0));
		node->children.resize(node->actions.size());
	}

	std::vector<double> chance_value(Node* node, std::vector<double> reach[2], int traverser, bool update, int depth) {
		int out_n = (traverser == 0) ? N : M;
		std::vector<double> total(out_n, 0.0);
		int nC = (int)node->chance_cards.size();
		// Same pre-creation-then-parallel-dispatch split as cfr()'s action
		// loop above (see kParallelDepthCutoff's comment): a real river
		// chance node here can have ~44-48 undealt cards, each an
		// independent subtree, making this an even richer parallelization
		// target than the (typically 2-4-way) action fan-out.
		for (int ci = 0; ci < nC; ci++) {
			if (!node->children[ci]) {
				unsigned char c = node->chance_cards[ci];
				node->children[ci].reset(new Node());
				node->children[ci]->state = node->state;
				node->children[ci]->board = node->board;
				node->children[ci]->board.push_back(c);
			}
		}
		auto child_utils = parallel_map(nC, depth, [&](int ci) {
			return [this, node, &reach, traverser, update, depth, ci]() {
				unsigned char c = node->chance_cards[ci];
				std::vector<double> new_reach[2] = { reach[0], reach[1] };
				for (int h = 0; h < N; h++) if (range_.hero[h][0] == c || range_.hero[h][1] == c) new_reach[0][h] = 0.0;
				for (int h = 0; h < M; h++) if (range_.villain[h][0] == c || range_.villain[h][1] == c) new_reach[1][h] = 0.0;
				return cfr(node->children[ci].get(), new_reach, traverser, update, depth + 1);
			};
		});
		for (int ci = 0; ci < nC; ci++) {
			const std::vector<double>& child_util = child_utils[ci];
			for (int h = 0; h < out_n; h++) total[h] += child_util[h] / nC;
		}
		return total;
	}

	std::vector<double> terminal_fold(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		int folder = s.table.players[0].active ? 1 : 0;
		int winner = 1 - folder;
		int other = 1 - traverser;
		double other_reach_sum = 0.0;
		for (double r : reach[other]) other_reach_sum += r;
		int out_n = (traverser == 0) ? N : M;
		double v = (traverser == winner)
			? (double)(s.table.total_pot - s.table.players[traverser].n_bet_chips())
			: -(double)s.table.players[traverser].n_bet_chips();
		return std::vector<double>(out_n, v * other_reach_sum);
	}

	std::vector<double> terminal_leaf(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		double pot = s.table.total_pot;
		int out_n = (traverser == 0) ? N : M;
		int other_n = (traverser == 0) ? M : N;

		// Build this node's (hero x villain) sign cache once (see Node's
		// comment above): leaf_->expected_showdown_sign(hi, vi) is a pure
		// function of the two combos and this node's board -- it does not
		// depend on reach or iteration, so it never needs recomputing after
		// the first visit to this node.
		if (!node->leaf_sign_cache_ready) {
			node->leaf_sign_cache.assign(N, std::vector<float>(M, 0.0f));
			for (int hi = 0; hi < N; hi++) {
				for (int vi = 0; vi < M; vi++) {
					if (!hands_compatible(range_.hero[hi], range_.villain[vi])) continue;
					node->leaf_sign_cache[hi][vi] = (float)leaf_->expected_showdown_sign(hi, vi);
				}
			}
			node->leaf_sign_cache_ready = true;
		}

		std::vector<double> util(out_n, 0.0);
		for (int th = 0; th < out_n; th++) {
			double v = 0.0;
			for (int oh = 0; oh < other_n; oh++) {
				double r = reach[1 - traverser][oh];
				if (r == 0.0) continue;
				int hi = (traverser == 0) ? th : oh;
				int vi = (traverser == 0) ? oh : th;
				const auto& hero_hand = range_.hero[hi];
				const auto& villain_hand = range_.villain[vi];
				if (!hands_compatible(hero_hand, villain_hand)) continue;
				double sign = (traverser == 0) ? node->leaf_sign_cache[hi][vi] : -node->leaf_sign_cache[hi][vi];
				v += r * sign * (pot / 2.0);
			}
			util[th] = v;
		}
		return util;
	}

	// TURN-mode analogue of terminal_leaf() above, using river_leaf_
	// (a RiverClusterLeafModel) instead of leaf_ (a TurnClusterLeafModel).
	// Fires once TURN betting closes (see cfr()/best_response()'s dispatch),
	// replacing what would otherwise be a real river chance-node expansion
	// (~48 branches) plus an exact showdown at every one of them, every
	// CFR iteration. Only used when river_leaf_ is non-null and
	// river_leaf_->available() -- see LiveResolver's constructor comment.
	std::vector<double> terminal_river_leaf(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		double pot = s.table.total_pot;
		int out_n = (traverser == 0) ? N : M;
		int other_n = (traverser == 0) ? M : N;

		// Same one-time cache idea as terminal_leaf() above, for river_leaf_
		// instead of leaf_ -- kept as a separate cache field since the two
		// functions are dispatched for different modes/board depths and are
		// never both valid for the same node.
		if (!node->river_leaf_sign_cache_ready) {
			node->river_leaf_sign_cache.assign(N, std::vector<float>(M, 0.0f));
			for (int hi = 0; hi < N; hi++) {
				for (int vi = 0; vi < M; vi++) {
					if (!hands_compatible(range_.hero[hi], range_.villain[vi])) continue;
					node->river_leaf_sign_cache[hi][vi] = (float)river_leaf_->expected_showdown_sign(hi, vi);
				}
			}
			node->river_leaf_sign_cache_ready = true;
		}

		std::vector<double> util(out_n, 0.0);
		for (int th = 0; th < out_n; th++) {
			double v = 0.0;
			for (int oh = 0; oh < other_n; oh++) {
				double r = reach[1 - traverser][oh];
				if (r == 0.0) continue;
				int hi = (traverser == 0) ? th : oh;
				int vi = (traverser == 0) ? oh : th;
				const auto& hero_hand = range_.hero[hi];
				const auto& villain_hand = range_.villain[vi];
				if (!hands_compatible(hero_hand, villain_hand)) continue;
				double sign = (traverser == 0) ? node->river_leaf_sign_cache[hi][vi] : -node->river_leaf_sign_cache[hi][vi];
				v += r * sign * (pot / 2.0);
			}
			util[th] = v;
		}
		return util;
	}

	std::vector<double> terminal_showdown(Node* node, std::vector<double> reach[2], int traverser) {
		Searchstate& s = node->state;
		double pot = s.table.total_pot;
		double traverser_contrib = s.table.players[traverser].n_bet_chips();
		int out_n = (traverser == 0) ? N : M;
		int other_n = (traverser == 0) ? M : N;
		unsigned char comm[5] = { node->board[0], node->board[1], node->board[2], node->board[3], node->board[4] };

		// Build this node's per-combo hand-strength cache once (see Node's
		// comment above): engine_->Maxstrength(hand, board) is a pure function
		// of one combo and this node's fixed board -- it does NOT depend on the
		// opponent's hand, reach, or iteration. The original code called it
		// twice (once per side) inside the O(N*M) pair loop below, so the same
		// hand's strength was recomputed redundantly against every one of the
		// opponent's combos, every iteration. Caching it per-combo, once,
		// collapses that to O(N+M) real hand evaluations total for this node's
		// entire lifetime; the O(N*M) loop below becomes a cheap integer
		// comparison, reproducing Engine::compute_winner()'s exact polarity
		// (lower Maxstrength = stronger hand -- see BUILD_NOTES section 22).
		if (!node->strength_cache_ready) {
			node->hero_strength_cache.assign(N, -1);
			for (int i = 0; i < N; i++) {
				const auto& h = range_.hero[i];
				if (collides_with_board(h, node->board)) continue;
				unsigned char hc[2] = { h[0], h[1] };
				node->hero_strength_cache[i] = engine_->Maxstrength(hc, comm);
			}
			node->villain_strength_cache.assign(M, -1);
			for (int j = 0; j < M; j++) {
				const auto& vh = range_.villain[j];
				if (collides_with_board(vh, node->board)) continue;
				unsigned char vc[2] = { vh[0], vh[1] };
				node->villain_strength_cache[j] = engine_->Maxstrength(vc, comm);
			}
			node->strength_cache_ready = true;
		}

		std::vector<double> util(out_n, 0.0);
		for (int th = 0; th < out_n; th++) {
			const auto& own_hand = (traverser == 0) ? range_.hero[th] : range_.villain[th];
			if (collides_with_board(own_hand, node->board)) { util[th] = 0.0; continue; }
			double v = 0.0;
			for (int oh = 0; oh < other_n; oh++) {
				double r = reach[1 - traverser][oh];
				if (r == 0.0) continue;
				const auto& hero_hand = (traverser == 0) ? range_.hero[th] : range_.hero[oh];
				const auto& villain_hand = (traverser == 0) ? range_.villain[oh] : range_.villain[th];
				const auto& other_hand = (traverser == 0) ? villain_hand : hero_hand;
				if (collides_with_board(other_hand, node->board)) continue;
				if (!hands_compatible(hero_hand, villain_hand)) continue;
				int hero_idx = (traverser == 0) ? th : oh;
				int villain_idx = (traverser == 0) ? oh : th;
				int hs = node->hero_strength_cache[hero_idx];
				int vs = node->villain_strength_cache[villain_idx];
				unsigned char w = (hs < vs) ? 0 : (hs > vs) ? 1 : 255; // matches Engine::compute_winner() exactly
				double val;
				if (w == 255) val = pot / 2.0 - traverser_contrib;
				else if ((int)w == traverser) val = pot - traverser_contrib;
				else val = -traverser_contrib;
				v += r * val;
			}
			util[th] = v;
		}
		return util;
	}

	const Players_range& range_;
	Engine* engine_;
	const TurnClusterLeafModel* leaf_;
	const RiverClusterLeafModel* river_leaf_;
	Mode mode_;
	bool extended_actions_;
	bool full_ladder_;
	int N, M;
};

} // namespace RealtimeSearch

