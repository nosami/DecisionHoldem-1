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
// subgame resolving: the opponent's range is a caller-supplied sample, not
// derived from a real range-tracking blueprint (which is unavailable -- see
// BUILD_NOTES.md). hero's range is typically just the single real hand held
// by whichever side is "me" in the live game (see dh_native_ai.cpp), which
// keeps N small and the resolve fast regardless of mode.
// ---------------------------------------------------------------------------
class LiveResolver {
public:
	enum class Mode { FLOP, TURN, RIVER };

	LiveResolver(const Players_range& range, Engine* eng, const TurnClusterLeafModel* leaf, Mode mode)
		: range_(range), engine_(eng), leaf_(leaf), mode_(mode) {
		N = (int)range_.hero.size();
		M = (int)range_.villain.size();
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
	};

	std::unique_ptr<Node> root;

	void init_root(const Searchstate& s0, const std::vector<unsigned char>& board0) {
		root.reset(new Node());
		root->state = s0;
		root->board = board0;
	}

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

	std::vector<double> cfr(Node* node, std::vector<double> reach[2], int traverser, bool update) {
		Searchstate& s = node->state;
		if (s.betting_stage == 5) return terminal_fold(node, reach, traverser);
		if (mode_ == Mode::FLOP && s.betting_stage >= 2) return terminal_leaf(node, reach, traverser);
		if (mode_ == Mode::TURN && (int)node->board.size() >= 5) return terminal_showdown(node, reach, traverser);

		expand(node);
		if (node->is_chance) return chance_value(node, reach, traverser, update);
		if (s.betting_stage >= 4) return terminal_showdown(node, reach, traverser);

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
				node->children[a]->board = node->board;
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
				const auto& hero_hand = (traverser == 0) ? range_.hero[th] : range_.hero[oh];
				const auto& villain_hand = (traverser == 0) ? range_.villain[oh] : range_.villain[th];
				if (!hands_compatible(hero_hand, villain_hand)) continue;
				double sign = (traverser == 0)
					? leaf_->expected_showdown_sign(th, oh)
					: -leaf_->expected_showdown_sign(oh, th);
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
	const TurnClusterLeafModel* leaf_;
	Mode mode_;
	int N, M;
};

} // namespace RealtimeSearch

