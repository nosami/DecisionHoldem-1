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
			if (hc > vc) sum += 1.0;
			else if (hc < vc) sum -= 1.0;
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

} // namespace RealtimeSearch
