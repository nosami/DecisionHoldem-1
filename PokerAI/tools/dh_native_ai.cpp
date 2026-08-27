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
//   SCOPE / HONEST LIMITATIONS (see BUILD_NOTES.md section 17/18 for the
//   full writeup):
//     - PREFLOP now uses the REAL trained blueprint (cluster/
//       blueprint_strategy.dat, ~16.1GB -- this file DOES exist in this
//       repo's data set; an earlier draft of this comment incorrectly
//       claimed it was unobtainable, which was wrong and has been
//       corrected) via PokerAI/tree/BlueprintReader.h, a new, targeted,
//       streaming reader that only reads the handful of node headers on
//       the path actually taken this hand -- never the whole ~16GB tree.
//       This works for the opening decision and for any preflop history
//       made only of calls/folds/allins/exactly-tree-modeled raise sizes.
//       If the history contains a raise whose size doesn't exactly match
//       one of the trained abstraction's discrete pot-fraction buckets (a
//       human GUI player can enter any arbitrary size), or if the file/
//       lookup fails for any reason, this falls back to the original,
//       clearly-labeled "call" placeholder for that decision only -- never
//       a guess. See BlueprintReader.h and BUILD_NOTES.md for the full
//       format writeup and honest validation status (this reader has not
//       been executed against the real file from within this development
//       sandbox, which lacks disk access to it -- see BUILD_NOTES.md).
//     - FLOP/TURN/RIVER decisions use LiveResolver (RealtimeSearch.h): a
//       small, fast, REDUCED-ACTION (fold / call / all-in only -- no
//       intermediate bet sizes) range-vs-range vanilla CFR resolve. The
//       opponent's range is NOT a fixed-size sample: LiveGame::villain_range
//       tracks a persistent, full (every remaining, non-board/hero-blocked
//       hole-card combo) weighted belief across the WHOLE hand, seeded from
//       the real trained preflop blueprint's per-cluster strategies and
//       narrowed, street by street, after every OBSERVED opponent action
//       (preflop: via a direct blueprint-probability Bayesian update;
//       postflop: via a dedicated LiveResolver run's own strat_sum output at
//       the exact node the opponent just acted from). This is still "unsafe"
//       resolving in the classical subgame-solving sense (no equilibrium
//       computation over hero's own strategy across the whole hand, just a
//       fresh vanilla-CFR resolve per decision against the current belief).
//       Postflop narrowing uses its OWN, separate resolver instance with one
//       extra genuine branch beyond hero's own fold/call/allin action set: a
//       canonical 1x-pot raise (native action byte 2). Hero's own live
//       decisions are unaffected (resolve_decision() always uses the
//       original 3-action resolver) -- this extra branch exists purely so
//       narrow_villain_range_postflop() has a real node to narrow an
//       observed non-all-in raise against, instead of silently skipping it
//       as earlier versions did. Any non-all-in raise size collapses onto
//       this single bucket (a min-raise and a 5x overbet narrow the same
//       way) -- the full native pot-fraction ladder was found computationally
//       infeasible for this resolver's chained turn/river chance-node fanout
//       (BUILD_NOTES.md section 16), so one extra branch is the tractable
//       middle ground actually implemented. See narrow_villain_range_postflop()
//       and RealtimeSearch.h's LiveResolver for the exact mechanics. Turn
//       decisions additionally assume the river gets checked down (no
//       river-betting subtree) purely to
//       keep response times interactive; river decisions are resolved
//       exactly (real showdown, no cluster approximation) since there are no
//       more cards to deal. See BUILD_NOTES.md for the design writeup and
//       measured performance cost of tracking a full (rather than a small,
//       fixed-size sampled) range.
//     - TURN mode's per-CFR-iteration cost (dealing a real river card via a
//       chance node, then an exact showdown, for every one of ~44-48
//       branches, every iteration) can optionally be replaced with a cheap
//       RiverClusterLeafModel lookup (BUILD_NOTES.md section 34) -- set the
//       DH_RIVER_SPLIT_DIR environment variable to the path of the
//       per-hole-hand split river-cluster files (see BUILD_NOTES.md section
//       31/34 for how those are built) to enable it. This is purely
//       opt-in/additive: unset (the default), or if the directory/files
//       can't be read, TURN mode transparently falls back to the original
//       exact chance-node + showdown behavior -- never worse or wrong,
//       only slower without it.
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
#include "../tree/BlueprintReader.h"
#include "../tree/PreflopCache.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <random>
#include <algorithm>
#include <chrono>

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
// A single candidate opponent hole-card combo and its current belief weight
// (not normalized to any fixed count -- LiveGame::villain_range holds every
// remaining, non-blocked combo for as long as it stays possible this hand).
struct WeightedHand {
	unsigned char c1, c2;
	double weight;
};

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

	// Persistent, full (no fixed pool size) opponent-range belief, tracked
	// for the opponent's slot (1 - my_id) across the WHOLE hand: every
	// hole-card combo not blocked by hero's own hole cards or the board,
	// each with a running belief weight. Initialized uniformly at
	// restart_game() (see init_villain_range()), pruned for newly-dealt
	// board cards at Next_stage() (see prune_villain_range_for_board()),
	// and Bayesian-narrowed after every observed opponent action (see
	// narrow_villain_range_preflop()/narrow_villain_range_postflop()).
	// Deliberately NOT capped at any fixed size -- see BUILD_NOTES.md for
	// the design rationale and measured performance cost.
	std::vector<WeightedHand> villain_range;

	// Real preflop blueprint bookkeeping (see BlueprintReader.h): the exact
	// sequence of action bytes (PokerAI/poker/State.h convention: 'd' fold,
	// 'l' call/check, 'n' allin, or a raise pot-fraction byte code) taken so
	// far this preflop street, from the tree's root. `preflop_path_confident`
	// goes false (permanently, for the rest of this hand) the moment a
	// raise is seen whose size can't be matched EXACTLY to one of the
	// trained abstraction's discrete byte codes -- at that point the path
	// can no longer be trusted, so preflop decisions fall back to the
	// original "call" placeholder rather than guess.
	std::vector<unsigned char> preflop_action_path;
	bool preflop_path_confident = true;
};

LiveGame g;

// Optional, purely-additive fast path for preflop blueprint lookups: a
// small (~750KB, measured) in-memory cache of every preflop-only node's
// trained strategy, built ahead of time by
// PokerAI/tools/build_preflop_cache.cpp (see that file and
// PokerAI/tree/PreflopCache.h for the full design). Loaded once here, at
// dylib load time, alongside the (much larger) global `engine` object
// this file already constructs eagerly the same way. If the cache file
// is missing or fails to load for any reason, `g_preflop_cache_loaded`
// stays false and every preflop lookup below transparently falls back to
// BlueprintReader's original per-decision disk walk (slower -- 6-10s per
// BUILD_NOTES.md section 23 -- but exactly as correct as before this
// feature existed). This can never make a decision WORSE or WRONG, only
// slower on a cache miss/failure.
PreflopCache::Cache g_preflop_cache;
bool g_preflop_cache_loaded = false;

struct PreflopCacheLoader {
	PreflopCacheLoader() {
		try {
			g_preflop_cache.load("cluster/preflop_blueprint_cache.bin");
			g_preflop_cache_loaded = true;
			std::fprintf(stderr,
				"[DH_PREFLOP_CACHE] loaded %zu preflop nodes from "
				"cluster/preflop_blueprint_cache.bin -- preflop lookups will "
				"use this in-memory cache instead of walking the ~16GB "
				"blueprint file per decision\n",
				g_preflop_cache.nodes.size());
		} catch (const std::exception& e) {
			g_preflop_cache_loaded = false;
			std::fprintf(stderr,
				"[DH_PREFLOP_CACHE] not available (%s) -- falling back to "
				"direct blueprint disk-walk lookups for every preflop decision "
				"(correct, but 6-10s slower per decision; run "
				"PokerAI/tools/build_preflop_cache once to build the cache and "
				"remove this slowdown)\n",
				e.what());
		}
	}
};
PreflopCacheLoader g_preflop_cache_loader;

// Optional, purely-additive fast path for TURN-mode decisions (BUILD_NOTES.md
// section 34): if the DH_RIVER_SPLIT_DIR environment variable is set to the
// path of the per-hole-hand split river-cluster files (one ~12.7MB
// "<handid>.bin" file per of the 1326 possible hole hands -- see section 31),
// TURN-mode LiveResolver runs use a RiverClusterLeafModel to estimate the
// river leaf value directly from precomputed cluster ids (a few dozen tiny
// disk reads per hand, done once per resolve, outside the CFR loop) instead
// of dealing a real river card and computing an exact showdown on every
// single CFR iteration. If unset, or if the directory/files can't actually
// be read, TURN mode transparently falls back to its original, exact
// chance-node + showdown behavior -- this can never make a TURN decision
// WORSE or WRONG, only slower when the split files aren't available.
std::string river_split_dir() {
	const char* env = std::getenv("DH_RIVER_SPLIT_DIR");
	return env ? std::string(env) : std::string();
}

// ---------------------------------------------------------------------------
// Optional, purely-additive verbose diagnostic logging: prints hero's real
// average-strategy distribution (every legal action's actual probability,
// not just whichever one gets sampled) and a compact summary of every
// villain_range narrowing update, to stderr. Off by default (matches this
// file's other opt-in DH_* env vars, e.g. DH_RIVER_SPLIT_DIR); enable with:
//   DH_VERBOSE_STRATEGY=1
// This is read-only instrumentation -- it never changes what action gets
// sampled/returned or how villain_range is narrowed, only what gets printed.
// Since Python's ctypes calls straight into this same process (no pipe/
// subprocess boundary), these stderr lines appear directly in whatever
// terminal/log is already capturing play_with_slumbot.py's own output (the
// same way the existing [DH_RANGE_MODEL]/[DH_PREFLOP_CACHE] messages
// already do) -- no changes to the Python driver are needed to see them.
bool dh_verbose_enabled() {
	const char* env = std::getenv("DH_VERBOSE_STRATEGY");
	return env && env[0] != '\0' && std::string(env) != "0";
}

// Renders a card id (this file's convention: id = suit*13 + rank, suits
// "scdh", ranks "23456789TJQKA", matching Visualize_Tree.h) as "Ts"/"Ah"/etc.
std::string dh_card_str(unsigned char c) {
	static const char suits[] = "scdh";
	static const char ranks[] = "23456789TJQKA";
	if (c >= 52) return "??";
	char buf[3] = { ranks[c % 13], suits[c / 13], '\0' };
	return std::string(buf);
}

// Human-readable name for one of this resolver's action bytes: 'd' fold,
// 'l' call/check, 'n' allin, anything else is a pot-fraction raise byte
// (byte/2.0 = the fraction of pot, per State.h's take_action() convention).
std::string dh_action_name(unsigned char act) {
	if (act == 'd') return "fold";
	if (act == 'l') return "call";
	if (act == 'n') return "allin";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "raise(%.2fx pot)", act / 2.0);
	return std::string(buf);
}

// Prints hero's full average-strategy distribution for the decision that's
// about to be sampled -- every legal action's real probability, plus the
// resolved subgame's measured exploitability, so a single sampled action
// (e.g. "allin") can be told apart from a 99%-certain shove vs. a 20%-of-
// the-time bluff. `label` distinguishes preflop vs. postflop/mode.
void dh_log_strategy(const char* label, const std::vector<unsigned char>& actions,
	const std::vector<double>& probs, double exploitability_pct, int pot) {
	if (!dh_verbose_enabled()) return;
	std::fprintf(stderr, "[DH_STRATEGY] %s hand=%s%s pot=%d expl=",
		label, dh_card_str(g.my_hole[0]).c_str(), dh_card_str(g.my_hole[1]).c_str(), pot);
	if (exploitability_pct < 0.0) std::fprintf(stderr, "n/a:"); // preflop: direct lookup, no CFR resolve here
	else std::fprintf(stderr, "%.2f%%:", exploitability_pct);
	for (size_t i = 0; i < actions.size(); i++)
		std::fprintf(stderr, " %s=%.2f%%", dh_action_name(actions[i]).c_str(), probs[i] * 100.0);
	std::fprintf(stderr, "\n");
}

// Prints a compact summary of a villain_range narrowing update: how
// concentrated the tracked belief was before/after (effective # of combos,
// via the inverse Herfindahl index 1/sum(w_i^2) -- a uniform range over N
// combos scores N, a range collapsed onto 1 combo scores 1), plus the
// top-5 most-weighted combos after the update. `weights_before` must be
// g.villain_range's weights captured immediately before narrowing (already
// normalized to sum to 1 from the previous step).
void dh_log_narrowing(const char* label, unsigned char observed_byte,
	const std::vector<double>& weights_before) {
	if (!dh_verbose_enabled()) return;
	auto effective_n = [](const std::vector<double>& w) {
		double sum_sq = 0.0;
		for (double x : w) sum_sq += x * x;
		return (sum_sq > 1e-15) ? 1.0 / sum_sq : 0.0;
	};
	double eff_before = effective_n(weights_before);
	std::vector<double> weights_after;
	weights_after.reserve(g.villain_range.size());
	for (auto& h : g.villain_range) weights_after.push_back(h.weight);
	double eff_after = effective_n(weights_after);

	std::vector<size_t> idx(g.villain_range.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	size_t top_k = std::min<size_t>(5, idx.size());
	std::partial_sort(idx.begin(), idx.begin() + top_k, idx.end(),
		[&](size_t a, size_t b) { return g.villain_range[a].weight > g.villain_range[b].weight; });

	std::fprintf(stderr,
		"[DH_RANGE_MODEL] %s narrow observed=%s combos=%zu effective_hands %.1f -> %.1f, top:",
		label, dh_action_name(observed_byte).c_str(), g.villain_range.size(), eff_before, eff_after);
	for (size_t k = 0; k < top_k; k++) {
		const WeightedHand& h = g.villain_range[idx[k]];
		std::fprintf(stderr, " %s%s=%.2f%%",
			dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0);
	}
	std::fprintf(stderr, "\n");
}

// Compares villain's REAL revealed hole cards (available at hand-end --
// Slumbot's API includes "bot_hole_cards" in the terminal response of
// EVERY hand, not just showdowns, per BUILD_NOTES.md) against the belief
// this run's own g.villain_range had settled on for them by that point in
// the hand. Reports the actual combo's rank and normalized weight among
// every combo this file was still tracking as possible, and flags it a
// "RANGE MISS" whenever that weight is below what a uniform guess over the
// remaining tracked combos would have assigned (i.e. our narrowing made
// this specific combo LESS likely than "no information at all" would have
// -- the concrete signature of "opponent wasn't holding a hand we thought
// was in his range"). Always prints (not gated behind DH_VERBOSE_STRATEGY):
// this is a single line per hand, directly answers "did narrowing mislead
// us this hand", and is useless if silently skipped on ordinary runs. Must
// be called (from the Python driver) after the real bot_hole_cards are
// known but BEFORE the next hand's restart_game() resets villain_range.
void dh_log_actual_hand(unsigned char c1, unsigned char c2) {
	if (g.villain_range.empty()) {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] actual villain hand=%s%s -- no tracked range "
			"available (villain_range empty); cannot compare\n",
			dh_card_str(c1).c_str(), dh_card_str(c2).c_str());
		return;
	}
	std::vector<size_t> idx(g.villain_range.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	std::sort(idx.begin(), idx.end(),
		[&](size_t a, size_t b) { return g.villain_range[a].weight > g.villain_range[b].weight; });

	int rank = -1;
	double actual_weight = 0.0;
	for (size_t k = 0; k < idx.size(); k++) {
		const WeightedHand& h = g.villain_range[idx[k]];
		if ((h.c1 == c1 && h.c2 == c2) || (h.c1 == c2 && h.c2 == c1)) {
			rank = (int)k + 1;
			actual_weight = h.weight;
			break;
		}
	}

	size_t n = g.villain_range.size();
	double uniform_weight = 1.0 / (double)n;
	size_t top_k = std::min<size_t>(5, n);
	std::string top_str;
	for (size_t k = 0; k < top_k; k++) {
		const WeightedHand& h = g.villain_range[idx[k]];
		char buf[32];
		std::snprintf(buf, sizeof(buf), " %s%s=%.2f%%",
			dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0);
		top_str += buf;
	}

	if (rank < 0) {
		// Should be impossible for a real, legal deal (villain_range covers
		// every non-blocked combo unless something upstream degenerately
		// collapsed to empty and was reset -- see
		// prune_villain_range_for_board()'s own "villain range was empty"
		// fallback warning). Reported as its own case rather than silently
		// treating it as rank N+1/weight 0, so a real tracking bug would be
		// obvious rather than just look like an extreme miss.
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] actual villain hand=%s%s NOT FOUND among %zu "
			"tracked combos -- RANGE MISS (unexpected: check for an earlier "
			"'villain range was empty' warning this hand). Top expected:%s\n",
			dh_card_str(c1).c_str(), dh_card_str(c2).c_str(), n, top_str.c_str());
		return;
	}

	bool is_miss = actual_weight < uniform_weight;
	std::fprintf(stderr,
		"[DH_RANGE_MODEL] actual villain hand=%s%s weight=%.4f%% rank=%d/%zu "
		"(uniform=%.4f%%) -- %s. Top expected:%s\n",
		dh_card_str(c1).c_str(), dh_card_str(c2).c_str(), actual_weight * 100.0, rank, n,
		uniform_weight * 100.0,
		is_miss ? "RANGE MISS (weighted BELOW a uniform random guess)" : "within expected range",
		top_str.c_str());
}

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

// pypokergui's "raise N" amount is always the TOTAL bet for the CURRENT
// street (mirrors pypokerengine's Player.paid_sum(), which is computed from
// action_histories that are cleared at the start of every new street) --
// but for PREFLOP specifically, the blind-posting entries are themselves
// part of that first street's action_histories, so "amount" already
// includes the blind. stack_at_street_start[] here is set (in
// reset_street_counters(), called from restart_game()) AFTER blinds are
// already deducted, so for every OTHER street it is the correct baseline to
// subtract "amount" from, but for preflop specifically the correct
// baseline is the ORIGINAL 20000 stack (subtracting from an
// already-blind-adjusted baseline would double-count the blind). This
// distinction only matters for parsing/emitting "raise N" strings.
int street_relative_raise_baseline(int slot) {
	return (g.betting_stage == 0) ? 20000 : g.stack_at_street_start[slot];
}

// Determines which discrete preflop raise byte-code (mirrors
// PokerAI/poker/State.h's take_action() EXACTLY) would produce the observed
// new whole-hand total bet for a player, given the true state immediately
// before their action. Returns -1 if no exact match exists (e.g. a human
// GUI player chose an arbitrary custom size the training abstraction never
// modeled) -- callers must treat that as "can't use the real blueprint for
// the rest of this preflop street," never round/guess a nearby bucket.
int match_raise_action_byte(int total_pot_before, int last_bigbet_before, int my_bet_before, int observed_new_total_bet) {
	int n_chips_to_call = last_bigbet_before - my_bet_before;
	int pot = total_pot_before + n_chips_to_call;
	static const int candidates[] = { 1, 2, 3, 4, 8, 20, 40 };
	for (int byte : candidates) {
		int last_raise = (byte != 3) ? (pot * byte / 200 * 100) : (pot / 400 * 100);
		if (last_bigbet_before + last_raise == observed_new_total_bet)
			return byte;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Persistent, full opponent-range belief tracking (LiveGame::villain_range).
//
// Unlike the earlier fixed-40-hand uniform sample this replaces, this is a
// weighted belief over EVERY currently-possible opponent hole-card combo,
// carried across the whole hand and narrowed after each observed opponent
// action -- preflop via the real trained blueprint's per-cluster
// strategies, postflop via a dedicated LiveResolver run's own strat_sum
// output. See BUILD_NOTES.md for the full design writeup, the
// button-vs-BB acting-order asymmetry it accounts for, and measured
// performance cost (a full-range resolve is markedly more expensive than
// the old 40-hand pool).
// ---------------------------------------------------------------------------

// Re-seeds villain_range to a uniform prior over every hole-card combo not
// blocked by hero's own two cards (1225 combos before any board is dealt).
// Called once per hand, from restart_game(), after g.my_hole is set.
void init_villain_range() {
	g.villain_range.clear();
	std::vector<unsigned char> deck;
	for (int c = 0; c < 52; c++) {
		if (c == g.my_hole[0] || c == g.my_hole[1]) continue;
		deck.push_back((unsigned char)c);
	}
	for (size_t i = 0; i < deck.size(); i++)
		for (size_t j = i + 1; j < deck.size(); j++)
			g.villain_range.push_back({ deck[i], deck[j], 0.0 });
	double w = 1.0 / (double)g.villain_range.size();
	for (auto& h : g.villain_range) h.weight = w;
}

// Permanently removes (not just zero-weights) any tracked combo that now
// collides with the board, then renormalizes. Called from Next_stage()
// whenever new board cards are dealt, so the tracked range keeps shrinking
// (both from card-removal AND from behavioral narrowing) rather than
// growing stale entries forever.
void prune_villain_range_for_board() {
	std::vector<WeightedHand> kept;
	kept.reserve(g.villain_range.size());
	double sum = 0.0;
	for (auto& h : g.villain_range) {
		bool collide = false;
		for (unsigned char b : g.board) if (h.c1 == b || h.c2 == b) { collide = true; break; }
		if (collide) continue;
		kept.push_back(h);
		sum += h.weight;
	}
	if (kept.empty()) {
		// Should not happen with real 52-card poker (there are always many
		// non-blocked combos left even on the river) -- but if some earlier
		// narrowing step degenerately zeroed everything out, fail safe by
		// falling back to a fresh uniform prior over the currently-legal
		// combos rather than leaving an empty range that would silently
		// break every subsequent resolve.
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] villain range was empty after board-collision pruning -- "
			"this should be impossible with a legal board/hole-card combination; "
			"resetting to a uniform prior over remaining combos\n");
		std::vector<unsigned char> deck;
		for (int c = 0; c < 52; c++) {
			if (c == g.my_hole[0] || c == g.my_hole[1]) continue;
			bool on_board = false;
			for (unsigned char b : g.board) if (b == c) { on_board = true; break; }
			if (on_board) continue;
			deck.push_back((unsigned char)c);
		}
		for (size_t i = 0; i < deck.size(); i++)
			for (size_t j = i + 1; j < deck.size(); j++)
				kept.push_back({ deck[i], deck[j], 0.0 });
		double w = kept.empty() ? 0.0 : 1.0 / (double)kept.size();
		for (auto& h : kept) h.weight = w;
	}
	else if (sum > 1e-12) {
		for (auto& h : kept) h.weight /= sum;
	}
	g.villain_range = std::move(kept);
}

// Bayesian-narrows villain_range using the REAL trained preflop blueprint:
// looks up every one of the 169 preflop hand clusters' probability of
// taking `observed_byte` at the node the opponent just acted from (i.e.
// g.preflop_action_path AS IT STOOD BEFORE this action was appended), then
// multiplies each tracked combo's weight by its own cluster's probability
// and renormalizes. Tries the in-memory PreflopCache first (microseconds,
// see PreflopCache.h) and falls back to a direct BlueprintReader disk walk
// (6-10s, see BUILD_NOTES.md section 23) only if the cache is unavailable
// or doesn't contain this exact path -- both paths are numerically
// identical (validated in tools/test_preflop_cache_validation.cpp), so this
// fallback is purely a speed difference, never a correctness difference.
// Every failure mode (file/lookup problems, an unrecognized action byte, a
// degenerate all-zero result) is caught and logged, leaving villain_range
// unchanged for this action only -- never a crash, never fabricated data,
// matching resolve_preflop_decision()'s own established error-handling
// style.
void narrow_villain_range_preflop(unsigned char observed_byte) {
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
				// to the disk walk below, exactly as if the cache weren't
				// loaded at all.
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
		std::vector<double> weights_before;
		if (dh_verbose_enabled()) {
			weights_before.reserve(g.villain_range.size());
			for (auto& h : g.villain_range) weights_before.push_back(h.weight);
		}
		double sum = 0.0;
		for (auto& h : g.villain_range) {
			unsigned char hand[2] = { h.c1, h.c2 };
			int cluster = engine->get_preflop_cluster(hand);
			double p = (cluster >= 0 && cluster < (int)res.probs.size()) ? res.probs[cluster][idx] : 0.0;
			h.weight *= p;
			sum += h.weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("villain range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : g.villain_range) h.weight /= sum;
		dh_log_narrowing("preflop", observed_byte, weights_before);
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] preflop villain-range narrowing failed (%s) -- "
			"range left unchanged for this action\n", e.what());
	}
}

// Builds the Searchstate snapshot for whichever slot is about to act, from
// the LiveGame fields AS THEY STAND RIGHT NOW (i.e. must be called before
// that action's own bookkeeping mutates them). Shared by resolve_decision()
// (hero's own real decision) and narrow_villain_range_postflop() (a
// dedicated resolve purely to extract the opponent's just-taken action's
// conditional probability per tracked hand, for narrowing).
Searchstate build_current_searchstate(int acting_slot) {
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
	s.player_i_index = (unsigned char)acting_slot;
	s.n_raises = (unsigned char)std::min(g.n_raises_this_street, 255);
	s.cur_round_action_num = (unsigned short)g.actions_this_street;
	// BUG FIX (BUILD_NOTES.md section 45): this field's name is
	// misleading -- State.h's own take_action() (see its "if (actionstr ==
	// 'l' && first_action_of_current_round)" round-closing check, and
	// reset_betting_round_state()'s "first_action_of_current_round =
	// false" at the start of every betting round) uses it to mean "has at
	// least one action ALREADY been taken this betting round" -- i.e. it
	// starts false and only becomes true AFTER an action is processed, so
	// that a check/call closes the round only on the SECOND such action
	// (once it comes back around), not the first. This used to be set
	// inverted here (true when g.actions_this_street==0, i.e. exactly
	// when NOBODY has acted yet) -- which made take_action() treat the
	// very FIRST check of any betting round as if it were the round-
	// closing second check, jumping straight from e.g. betting_stage=3
	// (river) to betting_stage=4 (showdown) the instant CFR's tree-walk
	// recursed into that action's child node, completely skipping the
	// other player's turn. Confirmed directly with a minimal Searchstate
	// reproduction: villain's opening river check went straight to
	// betting_stage=4 with this field true, vs. correctly staying at
	// betting_stage=3 (moving to the other player) with it false.
	// Every resolve rooted at the OPENING decision of a betting round --
	// both narrow_villain_range_postflop()'s narrowing (villain checks
	// first) and resolve_decision()'s own live decisions (whenever hero
	// is first to act) -- was affected, since build_current_searchstate()
	// is the sole source of this field for both callers.
	s.first_action_of_current_round = (g.actions_this_street == 0) ? 0 : 1;
	s.last_raise = (unsigned short)g.last_raise_size;
	return s;
}

// Adaptive convergence control: rather than a fixed CFR iteration count
// per street, run in small batches and stop once the MEASURED
// exploitability (LiveResolver::exploitability() -- the real best-response
// gap of the accumulated average strategy, weighted by the SAME villain-
// range belief actually used for this decision, not a synthetic uniform
// one) drops below TARGET_EXPLOITABILITY_PCT of the resolved subgame's
// pot, or a hard safety cap (iteration count or wall-clock time) is hit --
// whichever comes first. This replaces an earlier fixed-iteration-count
// design (FLOP=6000/TURN=300/RIVER=10000) once real measurement
// (tools/test_resolver_exploitability.cpp) showed exploitability isn't a
// clean, predictable function of iteration count alone -- it depends on
// the specific hand/board/range in front of the resolver and can even be
// mildly non-monotonic -- so a fixed count can't reliably promise "under 1%"
// the way actually checking the real quantity can.
//
// Real measured convergence curves (arbitrary synthetic full-range
// scenario, see tools/test_resolver_exploitability.cpp and BUILD_NOTES.md
// for the complete numbers and the earlier test-harness bug -- an
// uninitialized Searchstate field silently made every test scenario
// degenerate to a single legal action -- that had to be fixed before these
// numbers meant anything):
//   FLOP:  74.5% at 60 iters -> 3.48% at 1000 -> 1.13% at 4000 -> 0.80% at
//          6000 -> 0.54% at 10000. Reliably crosses 1% well within 10000.
//   RIVER: 75.3% at 60 -> 7.41% at 1000 -> 2.37% at 5000 -> 1.08% at 10000
//          -> crosses 1% somewhere around 12000-15000 (noisy after that:
//          0.01%/15000, 0.35%/20000, 0.72%/30000 -- vanilla CFR's average-
//          strategy exploitability isn't perfectly monotone run-to-run).
//   TURN:  still only down to ~3.3% after 2000 iterations (~7.8s at this
//          mode's ~4-6ms/iteration cost, since every TURN iteration must
//          enumerate all ~44 real river-card chance branches -- unlike
//          FLOP's TurnClusterLeafModel shortcut or RIVER's terminal
//          street). Genuinely reaching <1% for TURN this way would cost
//          many more seconds than is acceptable for live play, so TURN's
//          safety cap below is an explicit, disclosed "best effort"
//          compromise, NOT a claim that <1% is actually reached.
struct ConvergenceConfig {
	int batch_size;     // iterations run per exploitability check
	int max_iterations; // hard cap regardless of exploitability
	double max_ms;      // hard wall-clock cap regardless of exploitability or iteration count
};

ConvergenceConfig convergence_config_for_mode(LiveResolver::Mode mode, bool full_ladder = false) {
	// full_ladder widens hero's own action set at the opening decision of
	// a betting round from {fold, call, allin} to the real native
	// pot-fraction ladder (RealtimeSearch.h's LiveResolver constructor
	// comment / BUILD_NOTES.md section 37). Measured directly: this makes
	// each iteration meaningfully more expensive (more branches per node),
	// so the same time budgets used for the reduced 3-action tree are no
	// longer enough to reliably reach the same exploitability -- FLOP
	// measured 6.7% at the original 3000ms cap vs. 2.7% at 8000ms; RIVER
	// measured 1.97% at the original 6000ms cap vs. 0.96% (just crosses
	// 1%) at 10000ms. Both budgets below are widened ONLY when full_ladder
	// is active; the reduced-action (default) budgets are byte-for-byte
	// unchanged from before this feature existed. TURN's existing 12000ms
	// cap (section 35) was already measured to be enough for full_ladder
	// too (converged 0.92%-1.94% across repeated runs -- see BUILD_NOTES
	// section 37 for the honest caveat that it sometimes lands just over
	// 1%, same "best effort under a time cap" design the rest of this file
	// already uses), so TURN's cap is intentionally left unchanged here.
	if (mode == LiveResolver::Mode::FLOP)  return { 200, 10000, full_ladder ? 8000.0 : 3000.0 };
	// TURN's batch_size was 100 before the bet-size-narrowing fix added a 4th
	// (extended_actions) branch to this resolver's tree; the wall-clock cap
	// below is only checked BETWEEN batches, so a costlier per-iteration rate
	// makes any single batch's overshoot past max_ms bigger. Halved to 50 to
	// keep that overshoot bounded after the extra action made each iteration
	// more expensive (measured: batch=100 could overshoot the 12s cap by
	// ~2.7-2.8s/~23%, vs. the pre-existing ~1.2s/~10% overshoot at 3 actions
	// -- see BUILD_NOTES.md). This does not change what TURN converges TO,
	// only how precisely the safety cap is honored.
	//
	// max_iterations raised 2000 -> 20000 (BUILD_NOTES.md section 35): with
	// DH_RIVER_SPLIT_DIR set (RiverClusterLeafModel active, section 34), a
	// TURN iteration got ~20x cheaper, and the OLD 2000-iteration cap was
	// measured to be the binding constraint -- TURN hit it at only ~738ms of
	// wall-clock (far under the 12s max_ms) while still sitting at 3.4%
	// exploitability, never actually reaching the 1% target. Measured
	// directly against the real split files: raising the cap to 20000 lets
	// the same scenario run 4150 iterations / ~738ms and genuinely converge
	// under 1% (0.93%). Confirmed this raise is a no-op when the leaf model
	// is NOT active (DH_RIVER_SPLIT_DIR unset): the 12000ms wall-clock cap
	// still binds first at the same ~850 iterations/12.5s as before, byte-
	// for-byte identical to the old 2000-cap behavior in that case.
	if (mode == LiveResolver::Mode::TURN)  return { 50, 20000, 12000.0 };
	return { 500, 20000, full_ladder ? 10000.0 : 6000.0 }; // RIVER
}

const double TARGET_EXPLOITABILITY_PCT = 1.0;

// Runs `resolver` (already init_root()'d) in batches, seeding/continuing
// reach exactly as a single resolver.run(N, ...) call already would (CFR's
// regret/strat_sum accumulation lives on the persistent Node tree and is
// unaffected by being called across several smaller run() calls instead of
// one big one -- validated directly in tools/test_resolver_exploitability.cpp),
// stopping as soon as measured exploitability drops under
// TARGET_EXPLOITABILITY_PCT of the root pot or a safety cap is hit.
// `external_reach0`/`external_reach1` are passed straight through to both
// run() and exploitability() so the convergence check reflects this exact
// decision's real tracked-range belief, not a synthetic uniform one.
void run_until_converged(LiveResolver& resolver, LiveResolver::Mode mode,
	const std::vector<double>* external_reach0, const std::vector<double>* external_reach1,
	bool full_ladder = false) {
	ConvergenceConfig cfg = convergence_config_for_mode(mode, full_ladder);
	double pot = (double)resolver.root->state.table.total_pot;
	auto t0 = std::chrono::steady_clock::now();
	int done = 0;
	while (done < cfg.max_iterations) {
		int batch = std::min(cfg.batch_size, cfg.max_iterations - done);
		resolver.run(batch, external_reach0, external_reach1);
		done += batch;
		double expl_pct = (pot > 1e-9)
			? 100.0 * resolver.exploitability(external_reach0, external_reach1) / pot
			: 0.0;
		double elapsed_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
		if (expl_pct < TARGET_EXPLOITABILITY_PCT) break;
		if (elapsed_ms >= cfg.max_ms) break;
	}
}

// Bayesian-narrows villain_range using a DEDICATED LiveResolver run rooted
// at the state as it stood immediately before the opponent's just-observed
// postflop action (built via build_current_searchstate(opp_slot) -- must be
// called before that action's own stack/counter bookkeeping mutates
// LiveGame). This is a genuinely separate resolve from whatever hero's own
// most recent or next getdecision() call performs (see BUILD_NOTES.md for
// why: reusing a persisted resolve tree across separate C ABI calls was
// judged too fragile/bug-prone for the benefit, so this instead always
// re-resolves fresh, at real cost -- documented, not hidden).
//
// `observed_byte` must be one of the LiveResolver reduced action set ('d'
// fold, 'l' call/check, 'n' allin) -- an opponent's non-all-in postflop
// raise has no corresponding node in this reduced abstraction, so it can't
// be used to narrow the range; callers pass any other byte (e.g. '?') to
// make that skip explicit and logged rather than silently ignored.
void narrow_villain_range_postflop(int opp_slot, unsigned char observed_byte) {
	if (g.villain_range.empty()) return;
	if (observed_byte != 'd' && observed_byte != 'l' && observed_byte != 2 && observed_byte != 'n') {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] postflop villain-range narrowing skipped: action byte %d "
			"has no node in this resolver's action abstraction -- range left unchanged "
			"for this action\n", (int)observed_byte);
		return;
	}
	try {
		Searchstate s = build_current_searchstate(opp_slot);
		std::array<unsigned char, 2> my_hand = { g.my_hole[0], g.my_hole[1] };
		std::vector<std::array<unsigned char, 2>> tracked_hands;
		tracked_hands.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) tracked_hands.push_back({ h.c1, h.c2 });

		Players_range range;
		if (opp_slot == 0) { range.hero = tracked_hands; range.villain = { my_hand }; }
		else { range.hero = { my_hand }; range.villain = tracked_hands; }

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
		// extended_actions=true: this resolver instance is used ONLY to
		// compute a narrowing update, never to pick hero's own action (see
		// resolve_decision(), which always uses the default/false, 3-action
		// resolver instead) -- so it is safe to give it a genuine 4th
		// branch (byte 2, a canonical 1x-pot raise) so an observed non-
		// all-in raise has a real node to narrow against, instead of being
		// silently skipped. See RealtimeSearch.h's LiveResolver constructor
		// comment and BUILD_NOTES.md for the full design writeup and
		// measured cost of the extra action.
		LiveResolver resolver(range, engine, leaf.get(), mode, /*extended_actions=*/true, river_leaf.get());
		resolver.init_root(s, g.board);
		std::vector<double> tracked_weights;
		tracked_weights.reserve(g.villain_range.size());
		for (auto& h : g.villain_range) tracked_weights.push_back(h.weight);
		if (opp_slot == 0) run_until_converged(resolver, mode, &tracked_weights, nullptr);
		else run_until_converged(resolver, mode, nullptr, &tracked_weights);
		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

		double sum = 0.0;
		for (size_t i = 0; i < g.villain_range.size(); i++) {
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
			g.villain_range[i].weight *= avg[idx];
			sum += g.villain_range[i].weight;
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("villain range collapsed to ~0 total weight after this update -- refusing to apply");
		for (auto& h : g.villain_range) h.weight /= sum;
		// tracked_weights (captured above, before this update) doubles as
		// the "before" snapshot dh_log_narrowing needs -- no extra copy.
		dh_log_narrowing("postflop", observed_byte, tracked_weights);
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] postflop villain-range narrowing failed (%s) -- "
			"range left unchanged for this action\n", e.what());
	}
}

// Builds the Searchstate snapshot for the current decision, runs the
// appropriately-scoped LiveResolver against the LIVE, currently-tracked
// villain_range belief (not a fixed-size sample -- see above), and returns
// "fold" / "call" / "allin" sampled from hero's (my_id's) average strategy
// for my actual hole cards.
std::string resolve_decision() {
	Searchstate s = build_current_searchstate(g.my_id);

	std::array<unsigned char, 2> my_hand = { g.my_hole[0], g.my_hole[1] };
	std::vector<std::array<unsigned char, 2>> tracked_hands;
	std::vector<double> tracked_weights;
	tracked_hands.reserve(g.villain_range.size());
	tracked_weights.reserve(g.villain_range.size());
	for (auto& h : g.villain_range) {
		tracked_hands.push_back({ h.c1, h.c2 });
		tracked_weights.push_back(h.weight);
	}

	Players_range range;
	int opp_slot = 1 - g.my_id;
	if (opp_slot == 0) { range.hero = tracked_hands; range.villain = { my_hand }; }
	else { range.hero = { my_hand }; range.villain = tracked_hands; }

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

	// full_ladder gives hero's OWN decision the real native pot-fraction
	// bet sizes (0.5/1/2/4/10/20x pot, per State.h's legal_actions() --
	// the same abstraction the blueprint was trained with) at the opening
	// action of a betting round, instead of only fold/check/allin -- see
	// RealtimeSearch.h's LiveResolver constructor comment and BUILD_NOTES.md
	// section 37 for the full design writeup and measured tractability.
	// Only safe for modes that don't expand a further chance node inside
	// this resolver's own tree: FLOP and RIVER always qualify; TURN only
	// when river_leaf is actually active (non-null) -- TURN without it
	// still deals a real, expensive river chance node per iteration, and
	// combining that with the full ladder reproduces the original
	// "several minutes" combinatorial blowup this file used to warn
	// about, so it is deliberately excluded here.
	bool full_ladder = (mode == LiveResolver::Mode::FLOP) || (mode == LiveResolver::Mode::RIVER)
		|| (mode == LiveResolver::Mode::TURN && river_leaf != nullptr);

	LiveResolver resolver(range, engine, leaf.get(), mode, /*extended_actions=*/false, river_leaf.get(),
		full_ladder);
	resolver.init_root(s, g.board);
	if (opp_slot == 0) run_until_converged(resolver, mode, &tracked_weights, nullptr, full_ladder);
	else run_until_converged(resolver, mode, nullptr, &tracked_weights, full_ladder);
	// Adaptive iteration budget -- keeps iterating until measured
	// exploitability drops under ~1% of the pot (or a safety cap is hit);
	// see run_until_converged()'s comment above for the real measured
	// convergence data this replaces a fixed count with, and BUILD_NOTES.md
	// for the full writeup. Resolving against the full tracked range rather
	// than a fixed-size sample is markedly more expensive per iteration --
	// see BUILD_NOTES.md's range-model section for measured timings.

	// my_hand was placed at index 0 of whichever range list corresponds to
	// my slot (see above), and the root's acting player is always me (that
	// is the whole reason getdecision() was called), so index 0 always
	// addresses my own hand's strategy regardless of which slot I'm in.
	std::vector<double> avg;
	LiveResolver::average_strategy(resolver.root.get(), 0, avg);

	if (dh_verbose_enabled()) {
		double pot = (double)resolver.root->state.table.total_pot;
		double expl_pct = (pot > 1e-9)
			? 100.0 * resolver.exploitability(opp_slot == 0 ? &tracked_weights : nullptr,
				opp_slot == 0 ? nullptr : &tracked_weights) / pot
			: 0.0;
		const char* mode_name = (mode == LiveResolver::Mode::FLOP) ? "FLOP"
			: (mode == LiveResolver::Mode::TURN) ? "TURN" : "RIVER";
		dh_log_strategy(mode_name, resolver.root->actions, avg, expl_pct, (int)pot);
	}

	// Root actions may not literally be [fold, call, allin] in that order or
	// even all present (e.g., no fold offered if nothing is owed) -- match
	// by the actual encoded action bytes ('d'=fold, 'l'=call/check, 'n'=allin,
	// anything else is a pot-fraction raise byte -- only reachable when
	// full_ladder gave this decision access to the real ladder above).
	double r = std::uniform_real_distribution<double>(0.0, 1.0)(g.rng);
	double cum = 0.0;
	for (size_t a = 0; a < resolver.root->actions.size(); a++) {
		cum += avg[a];
		if (r <= cum || a + 1 == resolver.root->actions.size()) {
			unsigned char act = resolver.root->actions[a];
			if (act == 'd') return "fold";
			if (act == 'n') return "allin";
			if (act == 'l') return "call";
			// Pot-fraction raise byte: compute the real chip total using
			// the EXACT same formula State.h's take_action() uses to apply
			// this same byte, and resolve_preflop_decision() already uses
			// for its own raise bytes (last_raise = pot * byte / 200 * 100).
			// s.table.total_pot/s.last_bigbet (from build_current_searchstate())
			// are in the WHOLE-HAND (20000-baseline) convention this
			// resolver's own math (and the real State.h engine) uses
			// internally -- but apply_own_action()'s "raise N" parser
			// expects N in the STREET-RELATIVE convention for any street
			// other than preflop (street_relative_raise_baseline(); see its
			// own comment). Compute the whole-hand total first, then
			// convert down to street-relative by subtracting whatever was
			// already committed in EARLIER streets, exactly mirroring
			// opp_take_action()'s inverse (amount = street_relative_raise_
			// baseline(opp) - g.stack[opp]) for the same conversion in the
			// other direction.
			int total_pot_before = (int)s.table.total_pot;
			int last_bigbet_before = (int)s.last_bigbet;
			int my_bet_before = 20000 - g.stack[g.my_id];
			int n_chips_to_call = last_bigbet_before - my_bet_before;
			int pot = total_pot_before + n_chips_to_call;
			int last_raise = pot * act / 200 * 100;
			int new_total_bet_whole_hand = last_bigbet_before + last_raise;
			int already_committed_earlier_streets = 20000 - g.stack_at_street_start[g.my_id];
			int new_total_bet_street_relative = new_total_bet_whole_hand - already_committed_earlier_streets;
			return "raise " + std::to_string(new_total_bet_street_relative);
		}
	}
	return "call"; // defensive fallback, should be unreachable
}

// Facing hero's very first preflop decision, or having watched only
// calls/folds/allins/exactly-tree-modeled raises so far this preflop street
// (see LiveGame::preflop_path_confident), this queries the REAL trained CFR
// blueprint (cluster/blueprint_strategy.dat, via the new targeted
// BlueprintReader.h -- NOT the original Save_load.h full-tree loader) for
// hero's actual average strategy at this exact decision node, instead of
// the "always call" placeholder. Tries the in-memory PreflopCache first
// (microseconds; see PreflopCache.h/tools/build_preflop_cache.cpp) and
// falls back to a direct BlueprintReader disk walk (6-10s, see
// BUILD_NOTES.md section 23) only if the cache is unavailable or doesn't
// contain this exact path -- both are numerically identical (validated in
// tools/test_preflop_cache_validation.cpp), so this is purely a speed
// difference. Falls back further to the "always call" placeholder, for
// this decision only, if BOTH lookups fail for any reason (file missing/
// unreadable, path inconsistent with the tree, non-positive strategy sum,
// etc.) -- see BUILD_NOTES.md for the full honest writeup.
std::string resolve_preflop_decision() {
	if (!g.preflop_path_confident) {
		return "call"; // an earlier raise this street didn't match the trained
		                // abstraction's discrete sizing ladder -- see header.
	}
	try {
		int hand_cluster = engine->get_preflop_cluster(g.my_hole);
		BlueprintReader::LookupResult res;
		bool used_cache = false;
		if (g_preflop_cache_loaded) {
			try {
				res = PreflopCache::lookup_preflop_strategy(g_preflop_cache, g.preflop_action_path, hand_cluster);
				used_cache = true;
			} catch (const std::exception&) {
				// Cache miss/failure for this specific path -- fall through
				// to the disk walk below, exactly as if the cache weren't
				// loaded at all.
			}
		}
		if (!used_cache) {
			res = BlueprintReader::lookup_preflop_strategy(
				"cluster/blueprint_strategy.dat", g.preflop_action_path, hand_cluster);
		}

		int total_pot_before = (20000 - g.stack[0]) + (20000 - g.stack[1]);
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
		int my_bet_before = 20000 - g.stack[g.my_id];

		// Preflop has no live resolver/exploitability figure here (it's a
		// direct trained-blueprint lookup, not a CFR resolve) -- pass -1 as
		// a sentinel so dh_log_strategy's printed line reads "expl=n/a"
		// rather than a fabricated 0.00%.
		dh_log_strategy("PREFLOP", res.actionstr, res.probs, -1.0, total_pot_before);

		double r = std::uniform_real_distribution<double>(0.0, 1.0)(g.rng);
		double cum = 0.0;
		for (size_t a = 0; a < res.actionstr.size(); a++) {
			cum += res.probs[a];
			if (r <= cum || a + 1 == res.actionstr.size()) {
				unsigned char act = res.actionstr[a];
				g.preflop_action_path.push_back(act);
				if (act == 'd') return "fold";
				if (act == 'n') return "allin";
				if (act == 'l') return "call";
				// raise byte-code: compute the real chip total using the
				// EXACT same formula PokerAI/poker/State.h's take_action()
				// uses to apply this same byte.
				int n_chips_to_call = last_bigbet_before - my_bet_before;
				int pot = total_pot_before + n_chips_to_call;
				int last_raise = (act != 3) ? (pot * act / 200 * 100) : (pot / 400 * 100);
				int new_total_bet = last_bigbet_before + last_raise;
				return "raise " + std::to_string(new_total_bet);
			}
		}
		return "call"; // defensive, should be unreachable (probs sum to 1)
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[DH_PREFLOP_BLUEPRINT] real blueprint lookup failed (%s) -- "
			"falling back to placeholder 'call' for this decision only\n",
			e.what());
		return "call";
	}
}

void apply_own_action(const std::string& action) {
	int me = g.my_id;
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
	else if (action.rfind("raise ", 0) == 0) {
		int amount = std::stoi(action.substr(6));
		g.stack[me] = street_relative_raise_baseline(me) - amount;
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else { // "call" (also covers "check" -- identical bookkeeping when nothing is owed)
		// A call always brings the caller's WHOLE-HAND cumulative
		// contribution up to match whichever player has put in the most so
		// far -- that is simply what "call" means. This MUST use the raw
		// 20000 baseline (the same one every other whole-hand-cumulative
		// computation in this file uses -- e.g. resolve_preflop_decision(),
		// match_raise_action_byte(), resolve_decision()'s s.last_bigbet),
		// NOT g.stack_at_street_start[me]-prev_facing: on preflop
		// specifically, stack_at_street_start[] is already blind-adjusted
		// (see restart_game()), so that street-relative formula silently
		// no-ops the small blind's very first action (completing the blind
		// from 50 to 100), permanently under-counting the SB's
		// contribution by exactly the blind amount for the rest of the
		// hand -- which then makes last_bigbet/n_chips_to_call wrong on
		// every later street, occasionally causing legal_actions() to
		// wrongly still offer fold when the true amount owed is 0. See
		// BUILD_NOTES.md section 24.
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
		g.stack[me] = 20000 - last_bigbet_before;
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
	g.preflop_action_path.clear();
	g.preflop_path_confident = true;
	init_villain_range();
	reset_street_counters();
}

void Next_stage(int betting_stage, char* community_card_idx) {
	int lenc = betting_stage + 2;
	g.board.assign((unsigned char*)community_card_idx, (unsigned char*)community_card_idx + lenc);
	g.betting_stage = betting_stage;
	reset_street_counters();
	prune_villain_range_for_board();
}

void opp_take_action(char* actionstr_c) {
	std::string a(actionstr_c);
	int opp = 1 - g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	bool preflop = (g.betting_stage == 0);
	if (a == "fold") {
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('d'); }
		else narrow_villain_range_postflop(opp, 'd');
		g.folder = opp;
		g.betting_stage = 5;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('d');
	}
	else if (a == "allin") {
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('n'); }
		else narrow_villain_range_postflop(opp, 'n');
		g.stack[opp] = 0;
		g.has_allin = true;
		int amount = g.stack_at_street_start[opp];
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('n');
	}
	else if (a.rfind("raise ", 0) == 0) {
		int amount = std::stoi(a.substr(6));
		if (preflop && g.preflop_path_confident) {
			// See street_relative_raise_baseline()'s comment: this must use
			// the whole-hand-cumulative (20000 - stack) convention, matching
			// PokerAI/poker/State.h's own Pokerstate::n_bet_chips()/total_pot
			// bookkeeping, which never resets across streets.
			int total_pot_before = (20000 - g.stack[0]) + (20000 - g.stack[1]);
			int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
			int my_bet_before = 20000 - g.stack[opp];
			int byte = match_raise_action_byte(total_pot_before, last_bigbet_before, my_bet_before, amount);
			if (byte >= 0) {
				narrow_villain_range_preflop((unsigned char)byte);
				g.preflop_action_path.push_back((unsigned char)byte);
			}
			else g.preflop_path_confident = false; // can no longer trust the tracked path this hand
		}
		else if (!preflop) {
			// Postflop: an all-in-sized raise maps to byte 'n'; any other
			// (non-all-in) raise now maps to byte 2, a canonical 1x-pot
			// raise bucket that narrow_villain_range_postflop() resolves
			// with an EXTENDED action set for exactly this purpose (see
			// its own comment and RealtimeSearch.h's LiveResolver
			// constructor). This does not distinguish a min-raise from a
			// 5x overbet -- both collapse onto the same single bucket,
			// since that's the only non-all-in raise node this reduced
			// abstraction has room for -- but it means a real, sized
			// opponent raise now actually narrows the tracked range,
			// instead of being silently skipped as before. See
			// BUILD_NOTES.md for the full design writeup, including why a
			// single bucket (not the full native ladder) was chosen.
			bool would_be_allin = (street_relative_raise_baseline(opp) - amount) == 0;
			narrow_villain_range_postflop(opp, would_be_allin ? (unsigned char)'n' : (unsigned char)2);
		}
		g.stack[opp] = street_relative_raise_baseline(opp) - amount;
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.n_raises_this_street++;
		g.actions_this_street++;
	}
	else { // "call" or "check"
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('l'); }
		else narrow_villain_range_postflop(opp, 'l');
		// See apply_own_action()'s matching comment / BUILD_NOTES.md section
		// 24: must use the raw 20000 whole-hand baseline here, not
		// g.stack_at_street_start[opp]-prev_facing, or the small blind's
		// preflop completing call/limp silently no-ops.
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
		g.stack[opp] = 20000 - last_bigbet_before;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('l');
	}
}

void getdecision(char* out_buf) {
	std::memset(out_buf, 0, 20);
	std::string action;
	if (g.betting_stage == 0) {
		action = resolve_preflop_decision();
	}
	else {
		action = resolve_decision();
	}
	apply_own_action(action);
	std::strncpy(out_buf, action.c_str(), 19);
}

// Optional, purely-additive 5th ABI function: report villain's true
// revealed hole cards (card ids in this file's suit*13+rank convention,
// same as restart_game()'s c1id/c2id) at hand-end, for comparison against
// this run's own tracked villain_range belief. See dh_log_actual_hand()
// above for exactly what gets printed and why. Never affects any decision,
// narrowing update, or returned action -- read-only diagnostic logging
// only. Existing callers that don't call this (e.g. any other driver still
// using just the original 4 functions) are completely unaffected.
void report_actual_hand(int c1id, int c2id) {
	dh_log_actual_hand((unsigned char)c1id, (unsigned char)c2id);
}

} // extern "C"
