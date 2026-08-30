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
//     - By default FLOP/TURN/RIVER decisions use LiveResolver
//       (RealtimeSearch.h): a
//       small, fast, REDUCED-ACTION (fold / call / all-in only -- no
//       intermediate bet sizes) range-vs-range vanilla CFR resolve. The
//       both players' ranges are persistent full weighted beliefs across the
//       whole hand. They are seeded uniformly and narrowed, street by street,
//       after each player's public action
//       (preflop: via a direct blueprint-probability Bayesian update;
//       postflop: via a dedicated LiveResolver run's own strat_sum output at
//       the exact node the opponent just acted from). This is still "unsafe"
//       resolving in the classical subgame-solving sense (no equilibrium
//       computation over hero's own strategy across the whole hand, just a
//       fresh vanilla-CFR resolve per decision against the current belief).
//       Postflop narrowing uses its OWN, separate resolver instance with one
//       extra genuine branch beyond hero's own fold/call/allin action set: a
//       canonical 1x-pot raise (native action byte 2). In the default
//       resolver path, hero's own live decisions still use
//       resolve_decision()'s original action set -- this extra branch exists so
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
//       fixed-size sampled) range. DH_DIRECT_BLUEPRINT opts FLOP and TURN
//       into IndexedBlueprint.h's direct, positional-I/O policy lookup
//       instead -- default ON as of BUILD_NOTES.md section 52 (set
//       DH_DIRECT_BLUEPRINT=0 to restore the original LiveResolver-only
//       behavior). One public-tree cursor is advanced through preflop
//       and chance nodes; arbitrary opponent raises are pseudo-harmonically
//       translated only among that node's real raise actions, and the same
//       node's current-street bucket rows update villain_range. The source
//       and sidecar paths default to this user's real local data files,
//       /Users/jason/dh_local_data/blueprint_stgy.dat and
//       /Users/jason/dh_local_data/blueprint_stgy.dat.idx (BUILD_NOTES.md
//       section 52 -- previously cluster/blueprint_strategy.dat and
//       cluster/blueprint_strategy.dat.idx), and can be overridden with
//       DH_BLUEPRINT_PATH/DH_BLUEPRINT_INDEX (required for anyone else
//       building this repo on a different machine/username). Any failure
//       disables this cursor for the hand and transparently restores
//       LiveResolver.
//     - TURN mode's per-CFR-iteration cost (dealing a real river card via a
//       chance node, then an exact showdown, for every one of ~44-48
//       branches, every iteration) can optionally be replaced with a cheap
//       RiverClusterLeafModel lookup (BUILD_NOTES.md section 34) -- set the
//       DH_RIVER_SPLIT_DIR environment variable to the path of the
//       per-hole-hand split river-cluster files (see BUILD_NOTES.md section
//       31/34 for how those are built) to enable it. Default is this user's
//       real split-file directory, /Users/jason/dh_local_data/
//       river_cluster_split (BUILD_NOTES.md section 52 -- previously unset/
//       off), overridable with DH_RIVER_SPLIT_DIR for another machine/
//       username or a CI environment where that path doesn't exist. This
//       remains purely additive: if the directory/files can't be read
//       (default or overridden), TURN mode transparently falls back to the
//       original exact chance-node + showdown behavior -- never worse or
//       wrong, only slower without it.
//     - Purely-additive FALLBACK postflop resolver: resolve_decision() can
//       optionally reach for the external TexasSolver CFR solver
//       (PokerAI/tree/TexasSolverBridge.h, shelled out as a subprocess) if
//       the in-process LiveResolver path above throws, or (in "auto" mode)
//       converges too poorly. This NEVER replaces or changes LiveResolver's
//       own behavior on the success path -- see BUILD_NOTES.md's TexasSolver
//       section for the full design writeup, and TexasSolverBridge.h for the
//       wire-format details (verified directly against the actual checked-
//       out TexasSolver source, not guessed). RIVER ONLY (g.betting_stage==3):
//       a flop/turn-rooted TexasSolver solve has to enumerate every
//       remaining turn/river card runout with no leaf-value shortcut,
//       confirmed during validation to OOM-kill the subprocess at DH's
//       realistic range widths; a river-rooted solve has no further chance
//       nodes at all, so it stays cheap regardless of range width. FLOP/TURN
//       postflop decisions always use the in-process LiveResolver, completely
//       unaffected by any of the env vars below -- also the rare case in
//       practice, since resolve_direct_blueprint_decision() already answers
//       the large majority of them straight from the trained blueprint.
//       Controlled by:
//         DH_TEXASSOLVER_FALLBACK=force (default) | auto | off
//           (default changed from "auto" to "force" in BUILD_NOTES.md
//           section 52 -- set DH_TEXASSOLVER_FALLBACK=auto to restore the
//           original only-on-failure/high-exploitability behavior.)
//           auto: on the river only, use TexasSolver if the in-process
//                 resolver throws, or its measured exploitability exceeds
//                 DH_TEXASSOLVER_EXPLOITABILITY_TRIGGER_PCT (default 15.0 --
//                 a loose "something is clearly wrong" backstop, well above
//                 the in-process path's own 1% target).
//           force: on the river only, always use TexasSolver, skipping the
//                 in-process resolver entirely -- a testing/comparison hook,
//                 not intended for normal play. No effect on flop/turn.
//           off:   never use TexasSolver; if the in-process resolver fails,
//                 fall straight to the existing "call" placeholder pattern
//                 (see resolve_preflop_decision()'s own precedent).
//       DH_TEXASSOLVER_HOME / DH_TEXASSOLVER_BINARY / DH_TEXASSOLVER_
//       RESOURCE_DIR / DH_TEXASSOLVER_MAX_ITERATIONS / DH_TEXASSOLVER_THREADS
//       / DH_TEXASSOLVER_ACCURACY / DH_TEXASSOLVER_TIMEOUT_MS override the
//       solver's location and per-solve budget; see TexasSolverBridge.h's
//       load_config() for defaults. Scope limitation (see BUILD_NOTES.md):
//       only decisions with 0 or 1 prior actions this street are supported;
//       anything deeper falls through to the same "call" placeholder.
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
#include "../tree/IndexedBlueprint.h"
#include "../tree/BlueprintActionTranslation.h"
#include "../tree/TexasSolverBridge.h"
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
// A single candidate hole-card combo and its current belief weight.
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
	int blueprint_last_raise_size = 0;
	bool has_allin = false;
	int folder = -1; // slot that folded, or -1
	unsigned char my_hole[2] = { 0, 0 };
	std::vector<unsigned char> board;
	std::mt19937_64 rng{ std::random_device{}() };

	// Full public beliefs for our own and the opponent's ranges. hero_range
	// represents what our public actions reveal about our possible holdings;
	// villain_range represents our belief about the opponent. Both persist
	// across streets and are Bayesian-updated after their player's actions.
	std::vector<WeightedHand> hero_range;
	std::vector<WeightedHand> villain_range;

	// Ordered TexasSolver-vocabulary action tokens ("CHECK"/"CALL"/"FOLD"/
	// "BET_<n>"/"RAISE_<n>") for every action taken so far on the CURRENT
	// street, in chronological order. Cleared every street alongside
	// actions_this_street (see reset_street_counters()); appended to by
	// apply_own_action()/opp_take_action() below, using TexasSolver's own
	// wire vocabulary and increment convention -- NOT this struct's usual
	// pot-fraction byte codes. Only ever consumed by TexasSolverBridge.h's
	// solve() (river-only) as the `set_initial_actions` argument, which is
	// what lets a single TexasSolver solve answer ANY decision point this
	// street (not just the first two) by replaying this exact path. See
	// BUILD_NOTES.md for the full derivation, cross-checked against
	// TexasSolver's own src/GameTree.cpp action-tree construction.
	std::vector<std::string> street_action_path;

	// Real preflop blueprint bookkeeping (see BlueprintReader.h): the exact
	// sequence of action bytes (PokerAI/poker/State.h convention: 'd' fold,
	// 'l' call/check, 'n' allin, or a raise pot-fraction byte code) taken so
	// far this preflop street, from the tree's root. `preflop_path_confident`
	// goes false (permanently, for the rest of this hand) the moment a
	// raise is seen whose size can't be matched EXACTLY to one of the
	// trained abstraction's discrete byte codes -- at that point the path
	// can no longer be trusted, so preflop decisions fall back to the
	// original "call" placeholder rather than guess. The transition itself
	// is logged once, unconditionally, as "[DH_PREFLOP_BLUEPRINT] preflop
	// path confidence lost ..." (see opp_take_action()'s raise branch) --
	// this is the ONLY diagnostic for this fallback: resolve_preflop_
	// decision()'s own "!preflop_path_confident" guard has no strategy
	// distribution left to log, so unlike every other preflop/postflop
	// decision, a hand that hits this fallback prints no "[DH_STRATEGY]
	// PREFLOP ..." line at all -- that is expected, not a bug, once this
	// message is present to explain why.
	std::vector<unsigned char> preflop_action_path;
	bool preflop_path_confident = true;
	uint32_t blueprint_node = IndexedBlueprint::NO_CHILD;
	bool blueprint_cursor_usable = false;
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

std::unique_ptr<IndexedBlueprint::Reader> g_indexed_blueprint;
bool g_indexed_blueprint_init_attempted = false;

// Default ON (BUILD_NOTES.md section 52): the live SkyPoker bridge session
// always wants the direct indexed blueprint policy, so "unset" now means
// enabled instead of disabled -- matching dh_verbose_enabled()'s own
// unset-vs-explicit-override convention below. An explicit override still
// wins exactly as before: DH_DIRECT_BLUEPRINT=0 (or "false"/"FALSE") turns
// it back off, e.g. for a build/test harness that wants LiveResolver's
// original behavior instead.
bool direct_blueprint_enabled() {
	const char* value = std::getenv("DH_DIRECT_BLUEPRINT");
	if (!value) return true; // unset -> default on
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
		std::strcmp(value, "TRUE") == 0;
}

// Default is this machine's real trained blueprint data file (BUILD_NOTES.md
// section 52) -- NOT a path that ships with the repo or is portable to
// another machine/username. Anyone else building this repo (or a CI
// environment) should set DH_BLUEPRINT_PATH explicitly to their own copy;
// the env var, when set to anything, still wins over this default exactly
// as before.
std::string direct_blueprint_path() {
	const char* value = std::getenv("DH_BLUEPRINT_PATH");
	return value ? std::string(value) : std::string("/Users/jason/dh_local_data/blueprint_stgy.dat");
}

// No hardcoded personal-path default needed here: this derives from
// whatever direct_blueprint_path() resolved to (its own explicit override,
// or its new personal-path default above), so it automatically tracks
// the blueprint file it's actually indexing -- if DH_BLUEPRINT_PATH is
// overridden (another machine, another blueprint snapshot, a CI fixture)
// without also overriding DH_BLUEPRINT_INDEX, the sidecar index still
// correctly follows the overridden source instead of pointing at this
// user's personal index file. With both env vars unset this still resolves
// to exactly this user's real sidecar, /Users/jason/dh_local_data/
// blueprint_stgy.dat.idx, since source is now that same default path.
std::string direct_blueprint_index_path(const std::string& source) {
	const char* value = std::getenv("DH_BLUEPRINT_INDEX");
	return value ? std::string(value) : source + ".idx";
}

void initialize_direct_blueprint() {
	g.blueprint_cursor_usable = false;
	g.blueprint_node = IndexedBlueprint::NO_CHILD;
	if (!direct_blueprint_enabled()) return;
	if (!g_indexed_blueprint_init_attempted) {
		g_indexed_blueprint_init_attempted = true;
		try {
			std::string source = direct_blueprint_path();
			g_indexed_blueprint.reset(new IndexedBlueprint::Reader(
				source, direct_blueprint_index_path(source)));
			std::fprintf(stderr,
				"[DH_DIRECT_BLUEPRINT] indexed reader enabled: %zu decision nodes, "
				"bounded 32 MiB policy cache\n", g_indexed_blueprint->node_count());
		} catch (const std::exception& e) {
			std::fprintf(stderr,
				"[DH_DIRECT_BLUEPRINT] unavailable (%s) -- flop/turn will use LiveResolver\n",
				e.what());
		}
	}
	if (g_indexed_blueprint) {
		g.blueprint_node = g_indexed_blueprint->root();
		g.blueprint_cursor_usable = g.blueprint_node != IndexedBlueprint::NO_CHILD;
	}
}

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
// Default (BUILD_NOTES.md section 52) is this user's real, already-split
// local data directory -- not something that ships with the repo or is
// portable to another machine/username. Anyone else building this repo (or
// a CI environment) should set DH_RIVER_SPLIT_DIR explicitly to their own
// split-file directory (or leave it pointed at a nonexistent path, which
// safely falls back to the exact/original behavior described above); the
// env var, when set to anything, still wins over this default exactly as
// before.
std::string river_split_dir() {
	const char* env = std::getenv("DH_RIVER_SPLIT_DIR");
	return env ? std::string(env) : std::string("/Users/jason/dh_local_data/river_cluster_split");
}

// ---------------------------------------------------------------------------
// Optional, purely-additive verbose diagnostic logging: prints hero's real
// average-strategy distribution (every legal action's actual probability,
// not just whichever one gets sampled) and a compact summary of every
// villain_range narrowing update, to stderr. On by default as of
// BUILD_NOTES.md section 52 (the live SkyPoker bridge session always wants
// this visible, so "unset" now means enabled instead of disabled); disable
// explicitly with:
//   DH_VERBOSE_STRATEGY=0
// This is read-only instrumentation -- it never changes what action gets
// sampled/returned or how villain_range is narrowed, only what gets printed.
// Since Python's ctypes calls straight into this same process (no pipe/
// subprocess boundary), these stderr lines appear directly in whatever
// terminal/log is already capturing play_with_slumbot.py's own output (the
// same way the existing [DH_RANGE_MODEL]/[DH_PREFLOP_CACHE] messages
// already do) -- no changes to the Python driver are needed to see them.
bool dh_verbose_enabled() {
	const char* env = std::getenv("DH_VERBOSE_STRATEGY");
	if (!env) return true; // unset -> default on
	return env[0] != '\0' && std::string(env) != "0";
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

// Prints a compact summary of a range narrowing update: how
// concentrated the tracked belief was before/after (effective # of combos,
// via the inverse Herfindahl index 1/sum(w_i^2) -- a uniform range over N
// combos scores N, a range collapsed onto 1 combo scores 1), plus the
// top-5 most-weighted combos after the update. `weights_before` must be
void dh_log_narrowing(const char* label, unsigned char observed_byte,
	const std::vector<double>& weights_before,
	const std::vector<WeightedHand>& range) {
	if (!dh_verbose_enabled()) return;
	auto effective_n = [](const std::vector<double>& w) {
		double sum_sq = 0.0;
		for (double x : w) sum_sq += x * x;
		return (sum_sq > 1e-15) ? 1.0 / sum_sq : 0.0;
	};
	double eff_before = effective_n(weights_before);
	std::vector<double> weights_after;
	weights_after.reserve(range.size());
	for (const auto& h : range) weights_after.push_back(h.weight);
	double eff_after = effective_n(weights_after);

	std::vector<size_t> idx(range.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	size_t top_k = std::min<size_t>(5, idx.size());
	std::partial_sort(idx.begin(), idx.begin() + top_k, idx.end(),
		[&](size_t a, size_t b) { return range[a].weight > range[b].weight; });

	std::fprintf(stderr,
		"[DH_RANGE_MODEL] %s narrow observed=%s combos=%zu effective_hands %.1f -> %.1f, top:",
		label, dh_action_name(observed_byte).c_str(), range.size(), eff_before, eff_after);
	for (size_t k = 0; k < top_k; k++) {
		const WeightedHand& h = range[idx[k]];
		std::fprintf(stderr, " %s%s=%.2f%%",
			dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0);
	}
	std::fprintf(stderr, "\n");
}

// Same shape as dh_log_narrowing() above, but for a TexasSolver-fallback-
// driven narrowing update: the "observed" thing being narrowed against is
// HERO'S OWN just-sampled action, expressed as the same "fold"/"call"/
// "allin"/"raise <amount>" string resolve_decision() returns -- not one of
// this file's native pot-fraction action BYTES, so dh_action_name()/
// dh_log_narrowing() (which format a byte) can't be reused directly here.
void dh_log_texassolver_narrowing(const std::string& action_label,
	const std::vector<double>& weights_before,
	const std::vector<WeightedHand>& range) {
	if (!dh_verbose_enabled()) return;
	auto effective_n = [](const std::vector<double>& w) {
		double sum_sq = 0.0;
		for (double x : w) sum_sq += x * x;
		return (sum_sq > 1e-15) ? 1.0 / sum_sq : 0.0;
	};
	double eff_before = effective_n(weights_before);
	std::vector<double> weights_after;
	weights_after.reserve(range.size());
	for (const auto& h : range) weights_after.push_back(h.weight);
	double eff_after = effective_n(weights_after);

	std::vector<size_t> idx(range.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	size_t top_k = std::min<size_t>(5, idx.size());
	std::partial_sort(idx.begin(), idx.begin() + top_k, idx.end(),
		[&](size_t a, size_t b) { return range[a].weight > range[b].weight; });

	std::fprintf(stderr,
		"[DH_TEXASSOLVER] hero-postflop narrow observed=%s combos=%zu effective_hands %.1f -> %.1f, top:",
		action_label.c_str(), range.size(), eff_before, eff_after);
	for (size_t k = 0; k < top_k; k++) {
		const WeightedHand& h = range[idx[k]];
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

	// On a genuine miss (including the "not found" case below), print EVERY
	// tracked combo's weight, not just the top few -- a truncated top-5 list
	// can't show where the ACTUAL hand sat relative to the rest of the
	// distribution, or whether other similarly-shaped hands (e.g. other
	// combos that also make trips on this exact board) were *also*
	// underweighted, which is exactly the kind of pattern this diagnostic
	// exists to surface. Ordinary (non-miss) hands stay a compact top-5,
	// since there's nothing surprising to explain there.
	auto format_combo_list = [&](size_t count) {
		std::string s;
		for (size_t k = 0; k < count; k++) {
			const WeightedHand& h = g.villain_range[idx[k]];
			char buf[48];
			std::snprintf(buf, sizeof(buf), " #%zu %s%s=%.4f%%", k + 1,
				dh_card_str(h.c1).c_str(), dh_card_str(h.c2).c_str(), h.weight * 100.0);
			s += buf;
			if ((k + 1) % 8 == 0 && k + 1 < count) s += "\n   ";
		}
		return s;
	};
	std::string top_str = format_combo_list(std::min<size_t>(5, n));

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
			"'villain range was empty' warning this hand). All tracked "
			"combos, highest weight first:%s\n",
			dh_card_str(c1).c_str(), dh_card_str(c2).c_str(), n, format_combo_list(n).c_str());
		return;
	}

	bool is_miss = actual_weight < uniform_weight;
	std::fprintf(stderr,
		"[DH_RANGE_MODEL] actual villain hand=%s%s weight=%.4f%% rank=%d/%zu "
		"(uniform=%.4f%%) -- %s. %s:%s\n",
		dh_card_str(c1).c_str(), dh_card_str(c2).c_str(), actual_weight * 100.0, rank, n,
		uniform_weight * 100.0,
		is_miss ? "RANGE MISS (weighted BELOW a uniform random guess)" : "within expected range",
		is_miss ? "All tracked combos, highest weight first" : "Top expected",
		is_miss ? format_combo_list(n).c_str() : top_str.c_str());
}

int committed_this_street(int slot) {
	return g.stack_at_street_start[slot] - g.stack[slot];
}

// Small formatting helper for logging g.street_action_path (a comma is
// exactly the delimiter TexasSolver's own set_initial_actions expects, so
// this doubles as a preview of the literal string that would be sent).
std::string join_strings(const std::vector<std::string>& parts, const char* sep) {
	std::string out;
	for (size_t i = 0; i < parts.size(); i++) {
		if (i) out += sep;
		out += parts[i];
	}
	return out;
}

void reset_street_counters() {
	g.stack_at_street_start[0] = g.stack[0];
	g.stack_at_street_start[1] = g.stack[1];
	g.n_raises_this_street = 0;
	g.actions_this_street = 0;
	g.last_raise_size = 0;
	g.blueprint_last_raise_size = 0;
	g.street_action_path.clear();
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
// modeled). See match_raise_action_byte_fuzzy() below, which is what
// opp_take_action() actually calls -- this exact-match-only helper is kept
// as its first step and as the target of test_bet_size_narrowing's existing
// exact-match assertions.
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

// BUG FIX (BUILD_NOTES.md section 51; found investigating hand #12474088712,
// a live opponent's 6x-BB preflop open that doesn't hit any of
// match_raise_action_byte()'s seven exact discrete sizes). Previously, ANY
// non-exact preflop raise size permanently set g.preflop_path_confident =
// false for the rest of the hand's preflop street, and resolve_preflop_decision()
// silently returned a placeholder "call" -- with no [DH_STRATEGY] percentages
// logged and, critically, no dependence on hero's actual hand strength AT ALL
// -- for every one of hero's remaining preflop decisions this hand, regardless
// of how good or bad hero's cards were. This was a real decision-quality
// defect (not by-design; git blame shows it was never intentionally scoped to
// "only handle exact sizes"), and it is NOT rare: any live opponent whose bet
// sizing doesn't happen to land on one of the 7 trained multiples (a near
// certainty against human players, who don't size in exact fractions of pot)
// triggers it.
//
// The fix applies the SAME pseudo-harmonic bet-size-abstraction-translation
// technique already used, and already validated, for postflop raises (see
// BlueprintActionTranslation::translate() in ../tree/BlueprintActionTranslation.h,
// which itself uses ../tree/PseudoHarmonic.h) -- a standard, published action-
// translation method (Ganzfried & Sandholm, "Action Translation in Extensive-
// Form Games with Large Action Spaces", IJCAI 2013), already in production use
// elsewhere in this file. Instead of giving up when no exact byte matches, it
// brackets the observed raise between its two nearest trained sizes (by
// pot-relative fraction) and randomly samples one of the two, weighted by
// pseudo-harmonic interpolation toward whichever is closer -- so hero's
// subsequent preflop decisions still consult the real trained blueprint
// (hand-strength-aware, with real [DH_STRATEGY] percentages), instead of an
// always-identical placeholder "call". If the observed size falls entirely
// outside the trained ladder (bigger than the largest trained multiple, or
// smaller than the smallest -- effectively a near-min-raise or a huge
// overbet), it clamps to that nearest end bucket rather than extrapolating.
//
// This can still legitimately return -1 for a genuinely degenerate input (a
// non-positive pot or call amount, indicating a bookkeeping inconsistency
// upstream) -- that is NOT a sizing-abstraction issue and callers must still
// treat it as "can't trust the tracked path for the rest of this street."
template <typename RNG>
int match_raise_action_byte_fuzzy(int total_pot_before, int last_bigbet_before, int my_bet_before,
	int observed_new_total_bet, RNG& rng) {
	int exact = match_raise_action_byte(total_pot_before, last_bigbet_before, my_bet_before, observed_new_total_bet);
	if (exact >= 0) return exact;

	int n_chips_to_call = last_bigbet_before - my_bet_before;
	int pot = total_pot_before + n_chips_to_call;
	int observed_increment = observed_new_total_bet - last_bigbet_before;
	if (pot <= 0 || observed_increment <= 0) return -1; // degenerate, not a sizing issue

	struct Candidate { int byte; double fraction; };
	std::vector<Candidate> candidates;
	static const int bytes[] = { 1, 2, 3, 4, 8, 20, 40 };
	for (int b : bytes) {
		int last_raise = (b != 3) ? (pot * b / 200 * 100) : (pot / 400 * 100);
		if (last_raise <= 0) continue;
		candidates.push_back({ b, static_cast<double>(last_raise) / pot });
	}
	if (candidates.empty()) return -1;
	std::sort(candidates.begin(), candidates.end(),
		[](const Candidate& a, const Candidate& c) { return a.fraction < c.fraction; });

	double x = static_cast<double>(observed_increment) / pot;
	if (x <= candidates.front().fraction) return candidates.front().byte;
	if (x >= candidates.back().fraction) return candidates.back().byte;
	auto upper = std::upper_bound(candidates.begin(), candidates.end(), x,
		[](double value, const Candidate& c) { return value < c.fraction; });
	const Candidate& hi = *upper;
	const Candidate& lo = *(upper - 1);
	bool pick_lower = RealtimeSearch::randomized_pseudo_harmonic(lo.fraction, hi.fraction, x, rng);
	return pick_lower ? lo.byte : hi.byte;
}

BlueprintActionTranslation::BettingContext blueprint_betting_context(int acting_slot) {
	BlueprintActionTranslation::BettingContext context;
	context.total_pot = (20000 - g.stack[0]) + (20000 - g.stack[1]);
	context.max_commitment = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
	context.actor_commitment = 20000 - g.stack[acting_slot];
	context.actor_stack = g.stack[acting_slot];
	context.last_raise = g.blueprint_last_raise_size;
	return context;
}

void disable_blueprint_cursor(const std::string& reason) {
	if (g.blueprint_cursor_usable)
		std::fprintf(stderr,
			"[DH_DIRECT_BLUEPRINT] cursor disabled for this hand (%s) -- using LiveResolver\n",
			reason.c_str());
	g.blueprint_cursor_usable = false;
	g.blueprint_node = IndexedBlueprint::NO_CHILD;
}

void advance_blueprint_cursor(unsigned char action) {
	if (!g.blueprint_cursor_usable || !g_indexed_blueprint) return;
	uint32_t next = g_indexed_blueprint->child(g.blueprint_node, action);
	g.blueprint_node = next;
	if (next == IndexedBlueprint::NO_CHILD) g.blueprint_cursor_usable = false;
}

BlueprintActionTranslation::Translation translate_current_blueprint_action(
	int acting_slot,
	BlueprintActionTranslation::Kind kind,
	int observed_new_total)
{
	if (!g.blueprint_cursor_usable || !g_indexed_blueprint)
		throw std::runtime_error("indexed blueprint cursor is unavailable");
	return BlueprintActionTranslation::translate(
		g_indexed_blueprint->actions(g.blueprint_node), kind,
		blueprint_betting_context(acting_slot), observed_new_total, g.rng);
}

void track_blueprint_action(
	int acting_slot,
	BlueprintActionTranslation::Kind kind,
	int observed_new_total = 0)
{
	if (!g.blueprint_cursor_usable) return;
	try {
		auto translation = translate_current_blueprint_action(acting_slot, kind, observed_new_total);
		advance_blueprint_cursor(translation.sampled_action);
	} catch (const std::exception& e) {
		disable_blueprint_cursor(e.what());
	}
}

void track_exact_blueprint_action(unsigned char action) {
	if (!g.blueprint_cursor_usable) return;
	try {
		advance_blueprint_cursor(action);
	} catch (const std::exception& e) {
		disable_blueprint_cursor(e.what());
	}
}

uint32_t current_postflop_bucket(unsigned char c1, unsigned char c2) {
	unsigned char hand[2] = {c1, c2};
	if (g.betting_stage == 1) {
		if (g.board.size() < 3) throw std::runtime_error("flop board is incomplete");
		unsigned char board[3] = {g.board[0], g.board[1], g.board[2]};
		return engine->get_flop_cluster(hand, board);
	}
	if (g.betting_stage == 2) {
		if (g.board.size() < 4) throw std::runtime_error("turn board is incomplete");
		unsigned char board[4] = {g.board[0], g.board[1], g.board[2], g.board[3]};
		return engine->get_turn_cluster(hand, board);
	}
	throw std::runtime_error("direct blueprint bucket requested outside flop/turn");
}

void apply_direct_blueprint_likelihood(
	std::vector<WeightedHand>& range,
	const std::shared_ptr<const IndexedBlueprint::NodePolicy>& policy,
	const BlueprintActionTranslation::Translation& translation,
	const char* label)
{
	auto find_action = [&](unsigned char action) -> size_t {
		auto it = std::find(policy->actions.begin(), policy->actions.end(), action);
		if (it == policy->actions.end()) throw std::runtime_error("translated action absent from loaded policy");
		return static_cast<size_t>(it - policy->actions.begin());
	};
	size_t lower = find_action(translation.lower_action);
	size_t upper = find_action(translation.upper_action);
	std::vector<double> updated(range.size());
	std::vector<double> before;
	if (dh_verbose_enabled()) {
		before.reserve(range.size());
		for (const auto& hand : range) before.push_back(hand.weight);
	}
	double sum = 0.0;
	size_t action_count = policy->actions.size();
	for (size_t i = 0; i < range.size(); ++i) {
		const WeightedHand& hand = range[i];
		uint32_t bucket = current_postflop_bucket(hand.c1, hand.c2);
		if (bucket >= policy->bucket_count) throw std::runtime_error("hand bucket exceeds node dimension");
		const double* row = policy->probabilities.data() + static_cast<size_t>(bucket) * action_count;
		double likelihood = BlueprintActionTranslation::interpolated_probability(
			translation, row[lower], row[upper]);
		updated[i] = hand.weight * likelihood;
		sum += updated[i];
	}
	if (!(sum > 1e-12)) throw std::runtime_error("direct blueprint range update collapsed to zero");
	for (size_t i = 0; i < range.size(); ++i) range[i].weight = updated[i] / sum;
	dh_log_narrowing(label, translation.sampled_action, before, range);
}

bool narrow_villain_range_direct_blueprint(
	int opp_slot,
	BlueprintActionTranslation::Kind kind,
	int observed_new_total = 0)
{
	if (!g.blueprint_cursor_usable || !g_indexed_blueprint ||
		(g.betting_stage != 1 && g.betting_stage != 2)) return false;
	try {
		uint32_t node = g.blueprint_node;
		auto translation = translate_current_blueprint_action(opp_slot, kind, observed_new_total);
		auto policy = g_indexed_blueprint->all_rows(node);
		uint32_t expected_buckets = g.betting_stage == 1 ? 50000U : 5000U;
		if (policy->bucket_count != expected_buckets)
			throw std::runtime_error("current node has wrong street bucket dimension");
		apply_direct_blueprint_likelihood(g.villain_range, policy, translation,
			g.betting_stage == 1 ? "villain-flop-blueprint" : "villain-turn-blueprint");
		advance_blueprint_cursor(translation.sampled_action);
		return true;
	} catch (const std::exception& e) {
		disable_blueprint_cursor(e.what());
		return false;
	}
}

// ---------------------------------------------------------------------------
// Persistent, full range-belief tracking.
//
// Each belief is carried across the whole hand and narrowed after that
// player's actions: preflop via the trained blueprint and postflop via either
// the indexed blueprint or LiveResolver's average strategy.
// ---------------------------------------------------------------------------

void init_uniform_range(std::vector<WeightedHand>& range, bool block_actual_hand) {
	range.clear();
	std::vector<unsigned char> deck;
	for (int c = 0; c < 52; c++) {
		if (block_actual_hand && (c == g.my_hole[0] || c == g.my_hole[1])) continue;
		deck.push_back((unsigned char)c);
	}
	for (size_t i = 0; i < deck.size(); i++)
		for (size_t j = i + 1; j < deck.size(); j++)
			range.push_back({ deck[i], deck[j], 0.0 });
	double w = 1.0 / (double)range.size();
	for (auto& h : range) h.weight = w;
}

void init_ranges() {
	// Our public range cannot use our private cards as blockers; the opponent
	// range can, because our decisions condition on the hand we actually hold.
	init_uniform_range(g.hero_range, false);
	init_uniform_range(g.villain_range, true);
}

// Permanently removes board-colliding combos and renormalizes.
void prune_range_for_board(std::vector<WeightedHand>& range, const char* label,
	bool block_actual_hand) {
	std::vector<WeightedHand> kept;
	kept.reserve(range.size());
	double sum = 0.0;
	for (const auto& h : range) {
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
			"[DH_RANGE_MODEL] %s range was empty after board-collision pruning -- "
			"this should be impossible with a legal board/hole-card combination; "
			"resetting to a uniform prior over remaining combos\n", label);
		std::vector<unsigned char> deck;
		for (int c = 0; c < 52; c++) {
			if (block_actual_hand && (c == g.my_hole[0] || c == g.my_hole[1])) continue;
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
	else {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] %s range retained legal combos but no probability "
			"mass after board pruning; resetting those combos to a uniform prior\n",
			label);
		double w = 1.0 / (double)kept.size();
		for (auto& h : kept) h.weight = w;
	}
	range = std::move(kept);
}

void prune_ranges_for_board() {
	prune_range_for_board(g.hero_range, "hero", false);
	prune_range_for_board(g.villain_range, "villain", true);
}

std::vector<WeightedHand>& range_for_slot(int slot) {
	return slot == g.my_id ? g.hero_range : g.villain_range;
}

void build_resolver_ranges(Players_range& range,
	std::vector<double>& reach0, std::vector<double>& reach1) {
	const std::vector<WeightedHand>& slot0 = range_for_slot(0);
	const std::vector<WeightedHand>& slot1 = range_for_slot(1);
	range.hero.reserve(slot0.size());
	range.villain.reserve(slot1.size());
	reach0.reserve(slot0.size());
	reach1.reserve(slot1.size());
	for (const auto& h : slot0) {
		range.hero.push_back({h.c1, h.c2});
		reach0.push_back(h.weight);
	}
	for (const auto& h : slot1) {
		range.villain.push_back({h.c1, h.c2});
		reach1.push_back(h.weight);
	}
}

size_t find_hand_index(const std::vector<WeightedHand>& range,
	unsigned char c1, unsigned char c2) {
	if (c2 < c1) std::swap(c1, c2);
	for (size_t i = 0; i < range.size(); ++i)
		if (range[i].c1 == c1 && range[i].c2 == c2) return i;
	throw std::runtime_error("actual hero hand is absent from tracked hero range");
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
bool narrow_range_preflop(std::vector<WeightedHand>& range,
	unsigned char observed_byte, const char* label) {
	if (!g.preflop_path_confident) return false;
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
			weights_before.reserve(range.size());
			for (const auto& h : range) weights_before.push_back(h.weight);
		}
		double sum = 0.0;
		std::vector<double> updated(range.size());
		for (size_t i = 0; i < range.size(); ++i) {
			const WeightedHand& h = range[i];
			unsigned char hand[2] = { h.c1, h.c2 };
			int cluster = engine->get_preflop_cluster(hand);
			double p = (cluster >= 0 && cluster < (int)res.probs.size()) ? res.probs[cluster][idx] : 0.0;
			updated[i] = h.weight * p;
			sum += updated[i];
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("range collapsed to ~0 total weight after this update -- refusing to apply");
		for (size_t i = 0; i < range.size(); ++i) range[i].weight = updated[i] / sum;
		dh_log_narrowing(label, observed_byte, weights_before, range);
		return true;
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] %s preflop range narrowing failed (%s) -- "
			"range left unchanged for this action\n", label, e.what());
		return false;
	}
}

void narrow_villain_range_preflop(unsigned char observed_byte) {
	narrow_range_preflop(g.villain_range, observed_byte, "villain-preflop");
}

// Builds the Searchstate snapshot for whichever slot is about to act, from
// the LiveGame fields AS THEY STAND RIGHT NOW (i.e. must be called before
// that action's own bookkeeping mutates them). Shared by resolve_decision()
// (hero's own real decision) and narrow_villain_range_postflop().
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
// `out_final_expl_pct`, if non-null, receives the LAST measured
// exploitability-as-%-of-pot value from the loop below (whatever value
// triggered the stop, whether by hitting TARGET_EXPLOITABILITY_PCT or a
// safety cap) -- purely an additional reporting channel for a value this
// function already computes internally every batch regardless of caller;
// existing callers that don't pass it see no behavior change at all.
void run_until_converged(LiveResolver& resolver, LiveResolver::Mode mode,
	const std::vector<double>* external_reach0, const std::vector<double>* external_reach1,
	bool full_ladder = false, double* out_final_expl_pct = nullptr) {
	ConvergenceConfig cfg = convergence_config_for_mode(mode, full_ladder);
	double pot = (double)resolver.root->state.table.total_pot;
	auto t0 = std::chrono::steady_clock::now();
	int done = 0;
	double expl_pct = 0.0;
	while (done < cfg.max_iterations) {
		int batch = std::min(cfg.batch_size, cfg.max_iterations - done);
		resolver.run(batch, external_reach0, external_reach1);
		done += batch;
		expl_pct = (pot > 1e-9)
			? 100.0 * resolver.exploitability(external_reach0, external_reach1) / pot
			: 0.0;
		double elapsed_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
		if (expl_pct < TARGET_EXPLOITABILITY_PCT) break;
		if (elapsed_ms >= cfg.max_ms) break;
	}
	if (out_final_expl_pct) *out_final_expl_pct = expl_pct;
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
		Players_range range;
		std::vector<double> reach0, reach1;
		build_resolver_ranges(range, reach0, reach1);

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
		run_until_converged(resolver, mode, &reach0, &reach1);
		int idx = -1;
		for (size_t i = 0; i < resolver.root->actions.size(); i++)
			if (resolver.root->actions[i] == observed_byte) { idx = (int)i; break; }
		if (idx < 0)
			throw std::runtime_error("observed action not found among this node's resolved actions");

		std::vector<double> before;
		before.reserve(g.villain_range.size());
		for (const auto& h : g.villain_range) before.push_back(h.weight);
		std::vector<double> updated(g.villain_range.size());
		double sum = 0.0;
		for (size_t i = 0; i < g.villain_range.size(); i++) {
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)i, avg);
			updated[i] = g.villain_range[i].weight * avg[idx];
			sum += updated[i];
		}
		if (!(sum > 1e-12))
			throw std::runtime_error("villain range collapsed to ~0 total weight after this update -- refusing to apply");
		for (size_t i = 0; i < g.villain_range.size(); ++i)
			g.villain_range[i].weight = updated[i] / sum;
		dh_log_narrowing("villain-postflop", observed_byte, before, g.villain_range);
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[DH_RANGE_MODEL] postflop villain-range narrowing failed (%s) -- "
			"range left unchanged for this action\n", e.what());
	}
}

std::string resolve_direct_blueprint_decision() {
	if (!g.blueprint_cursor_usable || !g_indexed_blueprint ||
		(g.betting_stage != 1 && g.betting_stage != 2)) return std::string();
	try {
		uint32_t node = g.blueprint_node;
		const IndexedBlueprint::Entry& entry = g_indexed_blueprint->entry(node);
		uint32_t expected_buckets = g.betting_stage == 1 ? 50000U : 5000U;
		if (entry.bucket_count != expected_buckets)
			throw std::runtime_error("current node has wrong street bucket dimension");
		uint32_t bucket = current_postflop_bucket(g.my_hole[0], g.my_hole[1]);
		std::vector<unsigned char> actions = g_indexed_blueprint->actions(node);
		std::vector<double> probabilities = g_indexed_blueprint->row(node, bucket);
		if (actions.size() != probabilities.size() || actions.empty())
			throw std::runtime_error("invalid direct blueprint policy shape");
		dh_log_strategy(g.betting_stage == 1 ? "FLOP-BLUEPRINT" : "TURN-BLUEPRINT",
			actions, probabilities, -1.0,
			(20000 - g.stack[0]) + (20000 - g.stack[1]));

		double random = std::uniform_real_distribution<double>(0.0, 1.0)(g.rng);
		double cumulative = 0.0;
		unsigned char action = actions.back();
		for (size_t i = 0; i < actions.size(); ++i) {
			cumulative += probabilities[i];
			if (random <= cumulative || i + 1 == actions.size()) {
				action = actions[i];
				break;
			}
		}

		Searchstate state = build_current_searchstate(g.my_id);
		unsigned char legal[32];
		int legal_count = state.legal_actions(legal);
		if (std::find(legal, legal + legal_count, action) == legal + legal_count)
			throw std::runtime_error("sampled blueprint action is illegal in actual game state");

		std::string result;
		if (action == 'd') result = "fold";
		else if (action == 'l') result = "call";
		else if (action == 'n') result = "allin";
		else {
			auto context = blueprint_betting_context(g.my_id);
			int call = context.max_commitment - context.actor_commitment;
			int increment = BlueprintActionTranslation::raise_increment(context.total_pot + call, action);
			int whole_hand_total = context.max_commitment + increment;
			int earlier_streets = 20000 - g.stack_at_street_start[g.my_id];
			int street_total = whole_hand_total - earlier_streets;
			if (street_total <= committed_this_street(g.my_id) || street_total >= g.stack_at_street_start[g.my_id])
				throw std::runtime_error("sampled blueprint raise converts to invalid API amount");
			result = "raise " + std::to_string(street_total);
		}
		auto policy = g_indexed_blueprint->all_rows(node);
		BlueprintActionTranslation::Translation exact;
		exact.lower_action = action;
		exact.upper_action = action;
		exact.sampled_action = action;
		exact.lower_probability = 1.0;
		apply_direct_blueprint_likelihood(g.hero_range, policy, exact,
			g.betting_stage == 1 ? "hero-flop-blueprint" : "hero-turn-blueprint");
		advance_blueprint_cursor(action);
		return result;
	} catch (const std::exception& e) {
		disable_blueprint_cursor(e.what());
		return std::string();
	}
}

// Builds the Searchstate snapshot for the current decision, runs the
// appropriately-scoped LiveResolver against both live public range beliefs,
// then samples the strategy for our actual private hand.
//
// FALLBACK DESIGN (see this file's own top header comment for the
// DH_TEXASSOLVER_FALLBACK env var and TexasSolverBridge.h for the solver
// interface): the in-process computation above is done ENTIRELY into
// local variables (in_process_*) -- g.hero_range is not touched until
// after this function has decided, below, whether to keep that result or
// discard it in favor of a TexasSolver-driven one. This guarantees
// g.hero_range is narrowed EXACTLY ONCE per call, from exactly one
// resolver's real strategy, never partially from one and then again from
// the other. On the pure success path (no fallback ever considered) the
// sequence of computations and log output is byte-for-byte identical to
// this function's original, pre-fallback form.
//
// RIVER ONLY: the TexasSolver fallback is never even considered unless
// g.betting_stage==3 (river) -- see `fallback_eligible_street` below and
// TexasSolverBridge.h's top header comment for the full rationale (a
// flop/turn-rooted TexasSolver solve forces it to enumerate every
// remaining turn/river runout with no leaf-value shortcut, confirmed
// during validation to OOM-kill the subprocess at DH's realistic range
// widths; a river-rooted solve has no further chance nodes at all, so it
// stays cheap regardless of range width). FLOP/TURN postflop decisions
// are therefore handled EXCLUSIVELY by the in-process LiveResolver below,
// completely unchanged from before this integration existed -- this is
// also consistent with FLOP/TURN being the rare case in practice, since
// resolve_direct_blueprint_decision() (see getdecision()) already answers
// the large majority of them straight from the trained blueprint.
std::string resolve_decision() {
	texassolver_bridge::TriggerMode fallback_mode = texassolver_bridge::trigger_mode();
	bool fallback_eligible_street = (g.betting_stage == 3);

	bool in_process_ok = false;
	std::string in_process_result;
	std::vector<double> in_process_updated_weights;
	std::vector<double> in_process_before_weights;
	unsigned char in_process_act = 0;
	std::string in_process_error;
	double in_process_expl_pct = -1.0;

	// FORCE mode skips the in-process resolver entirely -- a deliberate
	// testing/comparison hook (see tools/test_texassolver_fallback.cpp),
	// not intended for normal play. Only takes effect on the river
	// (fallback_eligible_street); FORCE has no effect on flop/turn, which
	// always run the in-process resolver below regardless of this env var.
	if (!(fallback_mode == texassolver_bridge::TriggerMode::FORCE && fallback_eligible_street)) {
		try {
			Searchstate s = build_current_searchstate(g.my_id);

			Players_range range;
			std::vector<double> reach0, reach1;
			build_resolver_ranges(range, reach0, reach1);

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
			run_until_converged(resolver, mode, &reach0, &reach1, full_ladder, &in_process_expl_pct);
			// Adaptive iteration budget -- keeps iterating until measured
			// exploitability drops under ~1% of the pot (or a safety cap is hit);
			// see run_until_converged()'s comment above for the real measured
			// convergence data this replaces a fixed count with, and BUILD_NOTES.md
			// for the full writeup. Resolving against the full tracked range rather
			// than a fixed-size sample is markedly more expensive per iteration --
			// see BUILD_NOTES.md's range-model section for measured timings.
			// in_process_expl_pct now holds the same final exploitability value
			// this loop already computed internally on its last batch -- used
			// below both for the verbose log (as before) and for this
			// function's own AUTO fallback trigger, at no extra computation
			// cost (see run_until_converged()'s out_final_expl_pct comment).

			size_t my_hand_index = find_hand_index(g.hero_range, g.my_hole[0], g.my_hole[1]);
			std::vector<double> avg;
			LiveResolver::average_strategy(resolver.root.get(), (int)my_hand_index, avg);

			if (dh_verbose_enabled()) {
				double pot_d = (double)resolver.root->state.table.total_pot;
				const char* mode_name = (mode == LiveResolver::Mode::FLOP) ? "FLOP"
					: (mode == LiveResolver::Mode::TURN) ? "TURN" : "RIVER";
				dh_log_strategy(mode_name, resolver.root->actions, avg, in_process_expl_pct, (int)pot_d);
			}

			// Root actions may not literally be [fold, call, allin] in that order or
			// even all present (e.g., no fold offered if nothing is owed) -- match
			// by the actual encoded action bytes ('d'=fold, 'l'=call/check, 'n'=allin,
			// anything else is a pot-fraction raise byte -- only reachable when
			// full_ladder gave this decision access to the real ladder above).
			double r = std::uniform_real_distribution<double>(0.0, 1.0)(g.rng);
			double cum = 0.0;
			size_t selected_action = resolver.root->actions.size() - 1;
			for (size_t a = 0; a < resolver.root->actions.size(); a++) {
				cum += avg[a];
				if (r <= cum || a + 1 == resolver.root->actions.size()) {
					selected_action = a;
					break;
				}
			}

			in_process_before_weights.reserve(g.hero_range.size());
			for (const auto& h : g.hero_range) in_process_before_weights.push_back(h.weight);
			in_process_updated_weights.resize(g.hero_range.size());
			double sum = 0.0;
			for (size_t i = 0; i < g.hero_range.size(); ++i) {
				std::vector<double> hand_strategy;
				LiveResolver::average_strategy(resolver.root.get(), (int)i, hand_strategy);
				in_process_updated_weights[i] = g.hero_range[i].weight * hand_strategy[selected_action];
				sum += in_process_updated_weights[i];
			}
			if (!(sum > 1e-12))
				throw std::runtime_error("hero range collapsed to ~0 total weight after resolved action");
			for (size_t i = 0; i < in_process_updated_weights.size(); ++i)
				in_process_updated_weights[i] /= sum;
			in_process_act = resolver.root->actions[selected_action];

			if (in_process_act == 'd') in_process_result = "fold";
			else if (in_process_act == 'n') in_process_result = "allin";
			else if (in_process_act == 'l') in_process_result = "call";
			else {
				// Pot-fraction raise byte: compute the real chip total using the
				// EXACT same formula State.h uses, then convert it to the API's
				// street-relative amount.
				int total_pot_before = (int)s.table.total_pot;
				int last_bigbet_before = (int)s.last_bigbet;
				int my_bet_before = 20000 - g.stack[g.my_id];
				int n_chips_to_call = last_bigbet_before - my_bet_before;
				int pot_i = total_pot_before + n_chips_to_call;
				int last_raise = pot_i * in_process_act / 200 * 100;
				int new_total_bet_whole_hand = last_bigbet_before + last_raise;
				int already_committed_earlier_streets = 20000 - g.stack_at_street_start[g.my_id];
				int new_total_bet_street_relative = new_total_bet_whole_hand - already_committed_earlier_streets;
				in_process_result = "raise " + std::to_string(new_total_bet_street_relative);
			}
			in_process_ok = true;
		} catch (const std::exception& e) {
			in_process_error = e.what();
		} catch (...) {
			in_process_error = "unknown non-std::exception thrown from in-process resolver";
		}
	}

	// Decide whether to reach for the TexasSolver fallback -- see this
	// file's top header comment (DH_TEXASSOLVER_FALLBACK) for the three
	// modes' full semantics: FORCE always tries it; AUTO tries it only if
	// the in-process attempt above threw, or "succeeded" but converged
	// too poorly; OFF never tries it (want_fallback stays false). Gated
	// on fallback_eligible_street regardless of mode -- see this
	// function's own top comment and TexasSolverBridge.h for why FLOP/TURN
	// never reach for TexasSolver even under FORCE/AUTO.
	bool want_fallback = false;
	if (fallback_eligible_street) {
		if (fallback_mode == texassolver_bridge::TriggerMode::FORCE) want_fallback = true;
		else if (fallback_mode == texassolver_bridge::TriggerMode::AUTO) {
			if (!in_process_ok) want_fallback = true;
			else if (in_process_expl_pct >= texassolver_bridge::exploitability_trigger_pct()) want_fallback = true;
		}
	}

	if (want_fallback) {
		// set_pot's symmetric-commit assumption (see TexasSolverBridge.h)
		// requires both players to have committed EQUALLY as of this
		// street's start -- always true for a genuine street-start decision
		// (a betting round only closes once bets are matched), but checked
		// defensively rather than silently trusting it.
		if (g.stack_at_street_start[0] != g.stack_at_street_start[1]) {
			std::fprintf(stderr,
				"[DH_TEXASSOLVER] skipped: stack_at_street_start asymmetric (%d vs %d) -- "
				"cannot represent as a fresh symmetric-commit root\n",
				g.stack_at_street_start[0], g.stack_at_street_start[1]);
		} else {
			std::vector<texassolver_bridge::Combo> hero_combos, villain_combos;
			hero_combos.reserve(g.hero_range.size());
			for (const auto& h : g.hero_range) hero_combos.push_back({ h.c1, h.c2, h.weight });
			villain_combos.reserve(g.villain_range.size());
			for (const auto& h : g.villain_range) villain_combos.push_back({ h.c1, h.c2, h.weight });

			int pot_at_street_start = (20000 - g.stack_at_street_start[0]) + (20000 - g.stack_at_street_start[1]);
			int effective_stack_at_street_start = std::min(g.stack_at_street_start[0], g.stack_at_street_start[1]);

			if (dh_verbose_enabled())
				std::fprintf(stderr,
					"[DH_TEXASSOLVER] invoking fallback (mode=%s, in_process_ok=%d, in_process_expl_pct=%.2f%%, "
					"action_path=[%s])\n",
					fallback_mode == texassolver_bridge::TriggerMode::FORCE ? "force" : "auto",
					in_process_ok ? 1 : 0, in_process_expl_pct,
					join_strings(g.street_action_path, ",").c_str());

			texassolver_bridge::Decision fb = texassolver_bridge::solve(
				/*hero_is_ip=*/g.my_id == 0,
				hero_combos, villain_combos, g.board,
				pot_at_street_start, effective_stack_at_street_start,
				g.street_action_path,
				g.my_hole[0], g.my_hole[1], g.rng);

			if (fb.ok) {
				std::vector<double> before;
				before.reserve(g.hero_range.size());
				for (const auto& h : g.hero_range) before.push_back(h.weight);

				double sum = 0.0;
				std::vector<double> updated(g.hero_range.size());
				for (size_t i = 0; i < g.hero_range.size(); i++) {
					updated[i] = g.hero_range[i].weight * fb.hero_prob_of_chosen_action[i];
					sum += updated[i];
				}
				if (sum > 1e-12) {
					for (size_t i = 0; i < g.hero_range.size(); i++)
						g.hero_range[i].weight = updated[i] / sum;
					dh_log_texassolver_narrowing(fb.action, before, g.hero_range);
					if (dh_verbose_enabled() && !fb.diagnostic.empty())
						std::fprintf(stderr, "[DH_TEXASSOLVER] %s\n", fb.diagnostic.c_str());
					return fb.action;
				}
				std::fprintf(stderr,
					"[DH_TEXASSOLVER] fallback strategy collapsed hero range to ~0 weight -- "
					"discarding, trying remaining options\n");
			} else {
				std::fprintf(stderr, "[DH_TEXASSOLVER] fallback failed (%s)\n", fb.error.c_str());
			}
		}
	}

	// Either the fallback wasn't wanted (in-process succeeded well enough
	// and mode != FORCE), or it WAS wanted but itself failed/produced a
	// degenerate result -- a poorly-converged in-process result is still
	// preferable to the context-blind last-resort placeholder below, so
	// use it if we have it.
	if (in_process_ok) {
		for (size_t i = 0; i < g.hero_range.size(); ++i)
			g.hero_range[i].weight = in_process_updated_weights[i];
		dh_log_narrowing("hero-postflop", in_process_act, in_process_before_weights, g.hero_range);
		return in_process_result;
	}

	// Last resort: mirrors resolve_preflop_decision()'s own established
	// "always call" placeholder for this decision only, reached only when
	// EVERY other avenue (in-process resolver, and, if attempted, the
	// TexasSolver fallback) has failed. g.hero_range is deliberately left
	// UNTOUCHED here -- neither computation's narrowing update is
	// committed -- rather than guessing which (if either) partial/failed
	// computation to trust.
	std::fprintf(stderr,
		"[DH_RESOLVE] postflop decision unresolved by the in-process resolver%s -- "
		"falling back to placeholder 'call' for this decision only (%s)\n",
		(fallback_mode == texassolver_bridge::TriggerMode::FORCE && fallback_eligible_street)
			? " (skipped: forced fallback)" : "",
		in_process_error.empty() ? "no in-process error recorded" : in_process_error.c_str());
	return "call";
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
		track_blueprint_action(g.my_id, BlueprintActionTranslation::Kind::Call);
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
				narrow_range_preflop(g.hero_range, act, "hero-preflop");
				g.preflop_action_path.push_back(act);
				track_exact_blueprint_action(act);
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
		track_blueprint_action(g.my_id, BlueprintActionTranslation::Kind::Call);
		return "call"; // defensive, should be unreachable (probs sum to 1)
	}
	catch (const std::exception& e) {
		std::fprintf(stderr,
			"[DH_PREFLOP_BLUEPRINT] real blueprint lookup failed (%s) -- "
			"falling back to placeholder 'call' for this decision only\n",
			e.what());
		track_blueprint_action(g.my_id, BlueprintActionTranslation::Kind::Call);
		return "call";
	}
}

// Builds the TexasSolver-wire token ("BET_<n>" or "RAISE_<n>") for an
// aggressive (bet/raise/allin) action, mirroring EXACTLY how TexasSolver's
// own tree builder accumulates ip_commit/oop_commit (src/GameTree.cpp's
// "bet"/"raise" branches: `nextrule.ip_commit += one_betting_size` --
// always an INCREMENT on top of that seat's own prior commitment, never a
// new total). This is NOT the same quantity as DH's own "raise N" action
// string amount (always this actor's new TOTAL street-relative
// commitment, see street_relative_raise_baseline()'s comment) -- the two
// only coincide when the actor hadn't committed anything yet this street.
// "BET" names the FIRST aggressive action into a street with no live bet
// yet (prev_facing==0 immediately before this action); any aggressive
// action facing an existing bet is a "RAISE", regardless of how many
// raises have already happened this street. An all-in shove has no
// separate token in TexasSolver's own vocabulary (confirmed against
// include/nodes/GameTreeNode.h's PokerActions enum: BEGIN/ROUNDBEGIN/BET/
// RAISE/CHECK/FOLD/CALL only) -- it is just a BET/RAISE whose amount
// happens to be the actor's whole remaining stack, which TexasSolver's own
// nearest-available-size matching (PCfrSolver::navigateToSubtree) snaps to
// its configured all-in bucket, exactly as for any other sizing. See
// BUILD_NOTES.md for the full derivation and citations.
std::string texassolver_bet_or_raise_token(int prev_facing, int actor_committed_before, int new_total_commitment) {
	int increment = std::max(0, new_total_commitment - actor_committed_before);
	return (prev_facing == 0 ? "BET_" : "RAISE_") + std::to_string(increment);
}

// CHECK when nothing was owed immediately before this action, CALL
// otherwise -- TexasSolver's tree distinguishes these as different action
// names (unlike DH's own internal "call" string, which covers both).
inline const char* texassolver_check_or_call_token(int prev_facing) {
	return prev_facing == 0 ? "CHECK" : "CALL";
}

void apply_own_action(const std::string& action) {
	int me = g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	int me_committed_before = committed_this_street(me);
	if (action == "fold") {
		g.folder = me;
		g.betting_stage = 5;
		g.street_action_path.push_back("FOLD");
	}
	else if (action == "allin") {
		g.blueprint_last_raise_size = std::max(0, g.stack_at_street_start[me] - prev_facing);
		g.stack[me] = 0;
		g.has_allin = true;
		g.n_raises_this_street++;
		g.actions_this_street++;
		g.street_action_path.push_back(
			texassolver_bet_or_raise_token(prev_facing, me_committed_before, g.stack_at_street_start[me]));
	}
	else if (action.rfind("raise ", 0) == 0) {
		int amount = std::stoi(action.substr(6));
		g.blueprint_last_raise_size = std::max(0, amount - prev_facing);
		g.stack[me] = street_relative_raise_baseline(me) - amount;
		g.n_raises_this_street++;
		g.actions_this_street++;
		g.street_action_path.push_back(
			texassolver_bet_or_raise_token(prev_facing, me_committed_before, amount));
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
		g.street_action_path.push_back(texassolver_check_or_call_token(prev_facing));
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
	init_ranges();
	reset_street_counters();
	initialize_direct_blueprint();
}

void Next_stage(int betting_stage, char* community_card_idx) {
	int lenc = betting_stage + 2;
	g.board.assign((unsigned char*)community_card_idx, (unsigned char*)community_card_idx + lenc);
	g.betting_stage = betting_stage;
	reset_street_counters();
	prune_ranges_for_board();
	if (g.blueprint_cursor_usable && g_indexed_blueprint && betting_stage <= 2) {
		try {
			uint32_t expected = betting_stage == 1 ? 50000U : 5000U;
			if (g_indexed_blueprint->entry(g.blueprint_node).bucket_count != expected)
				throw std::runtime_error("street transition did not reach expected chance-collapsed node");
		} catch (const std::exception& e) {
			disable_blueprint_cursor(e.what());
		}
	}
}

void opp_take_action(char* actionstr_c) {
	std::string a(actionstr_c);
	int opp = 1 - g.my_id;
	int prev_facing = std::max(committed_this_street(0), committed_this_street(1));
	int opp_committed_before = committed_this_street(opp);
	bool preflop = (g.betting_stage == 0);
	if (a == "fold") {
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('d'); }
		else if (!narrow_villain_range_direct_blueprint(
			opp, BlueprintActionTranslation::Kind::Fold))
			narrow_villain_range_postflop(opp, 'd');
		if (preflop) track_blueprint_action(opp, BlueprintActionTranslation::Kind::Fold);
		g.folder = opp;
		g.betting_stage = 5;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('d');
		g.street_action_path.push_back("FOLD");
	}
	else if (a == "allin") {
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('n'); }
		else if (!narrow_villain_range_direct_blueprint(
			opp, BlueprintActionTranslation::Kind::AllIn))
			narrow_villain_range_postflop(opp, 'n');
		if (preflop) track_blueprint_action(opp, BlueprintActionTranslation::Kind::AllIn);
		g.stack[opp] = 0;
		g.has_allin = true;
		int amount = g.stack_at_street_start[opp];
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.blueprint_last_raise_size = g.last_raise_size;
		g.n_raises_this_street++;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('n');
		g.street_action_path.push_back(
			texassolver_bet_or_raise_token(prev_facing, opp_committed_before, g.stack_at_street_start[opp]));
	}
	// BUG FIX (found investigating live hand #12475294621, see BUILD_NOTES.md):
	// decisionholdem_bridge.py's opponent_action() has, for some time, been
	// able to send this exact "allin <amount>" command (a real, stack-diff-
	// corrected whole-hand-cumulative commitment, used instead of the bare
	// "allin" below whenever a reliable real-stack amount is known -- see
	// its own comment, which explicitly names this branch) -- but this
	// engine never actually implemented it. Since "allin 10840" matches
	// neither the exact "allin" check above nor the "raise " prefix check
	// below, it fell all the way through to the final call/check branch,
	// silently mis-recording a genuine opponent all-in as a plain call: the
	// tracked g.preflop_action_path got a phantom 'l' byte instead of the
	// real 'n' byte. For hand #12475294621 (villain shoved EUR10.84 over
	// hero's 3xBB open), this desynced the tracked path from the real
	// trained tree just enough that the next lookup (for hero's own
	// resulting decision) walked into an unrelated chance-node subtree and
	// threw BlueprintReader's "encountered a chance-node marker" exception,
	// falling back to a hardcoded, hand-strength-blind "call" -- exactly
	// the failure mode the bare "allin" path (and section 51's fix) exist
	// to avoid. This branch is identical to the bare "allin" branch above
	// except it uses the caller's real amount (already in native chip
	// units) instead of assuming the opponent shoved this engine's
	// fictional stack_at_street_start[opp] baseline (every hand is
	// internally modeled with a fixed 20000-chip stack) -- see the
	// existing "allin <amount>" comment in decisionholdem_bridge.py, which
	// this now actually implements.
	else if (a.rfind("allin ", 0) == 0) {
		int amount = std::stoi(a.substr(6));
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('n'); }
		else if (!narrow_villain_range_direct_blueprint(
			opp, BlueprintActionTranslation::Kind::AllIn))
			narrow_villain_range_postflop(opp, 'n');
		if (preflop) track_blueprint_action(opp, BlueprintActionTranslation::Kind::AllIn);
		// Unlike the bare "allin" branch above (which has no real number and
		// must assume the opponent shoved this engine's entire fictional
		// stack_at_street_start[opp] baseline), the whole point of the
		// caller supplying a real amount here is to represent a genuinely
		// short (in real-money terms) opponent stack accurately -- so the
		// pot/stack bookkeeping must actually USE it, exactly like the
		// "raise " branch just below does for a non-all-in raise, rather
		// than always collapsing to 0. Using street_relative_raise_baseline()
		// here (not a hardcoded 20000) mirrors the "raise " branch exactly,
		// including its preflop-vs-postflop whole-hand-vs-this-street
		// distinction (see that function's own comment).
		g.stack[opp] = street_relative_raise_baseline(opp) - amount;
		g.has_allin = true;
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.blueprint_last_raise_size = g.last_raise_size;
		g.n_raises_this_street++;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('n');
		g.street_action_path.push_back(
			texassolver_bet_or_raise_token(prev_facing, opp_committed_before, amount));
	}
	else if (a.rfind("raise ", 0) == 0) {
		int amount = std::stoi(a.substr(6));
		int observed_whole_hand_total = amount;
		if (!preflop)
			observed_whole_hand_total += 20000 - g.stack_at_street_start[opp];
		if (preflop && g.preflop_path_confident) {
			// See street_relative_raise_baseline()'s comment: this must use
			// the whole-hand-cumulative (20000 - stack) convention, matching
			// PokerAI/poker/State.h's own Pokerstate::n_bet_chips()/total_pot
			// bookkeeping, which never resets across streets.
			int total_pot_before = (20000 - g.stack[0]) + (20000 - g.stack[1]);
			int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
			int my_bet_before = 20000 - g.stack[opp];
			// match_raise_action_byte_fuzzy() (BUILD_NOTES.md section 51) only
			// returns -1 for a genuinely degenerate pot/call bookkeeping state
			// now, not merely an off-abstraction size -- an off-ladder size
			// (the common case against real opponents) is bracketed/sampled
			// via pseudo-harmonic interpolation instead, exactly like the
			// existing postflop raise-size translation.
			int byte = match_raise_action_byte_fuzzy(total_pot_before, last_bigbet_before, my_bet_before, amount, g.rng);
			if (byte >= 0) {
				narrow_villain_range_preflop((unsigned char)byte);
				g.preflop_action_path.push_back((unsigned char)byte);
			}
			else {
				// Mirrors disable_blueprint_cursor()'s sibling diagnostic for the
				// flop/turn indexed-blueprint cursor: log once, unconditionally
				// (not gated behind DH_VERBOSE_STRATEGY -- this is a real, if rare,
				// degradation event, not routine strategy-distribution detail),
				// exactly at the transition, since resolve_preflop_decision()'s own
				// "!preflop_path_confident" guard (this flag's only other reader)
				// returns its placeholder 'call' silently and has no strategy
				// distribution of its own left to print. This branch should now be
				// rare: it only fires for a degenerate pot/call bookkeeping state
				// (see match_raise_action_byte_fuzzy()'s comment), not for an
				// ordinary off-ladder bet size, which is handled above instead.
				std::fprintf(stderr,
					"[DH_PREFLOP_BLUEPRINT] preflop path confidence lost for the "
					"rest of this hand -- observed raise to %d hit a degenerate "
					"pot/call bookkeeping state that pseudo-harmonic size "
					"translation could not bracket -- preflop decisions will fall "
					"back to a placeholder 'call' with no [DH_STRATEGY] "
					"percentages logged\n",
					amount);
				g.preflop_path_confident = false; // can no longer trust the tracked path this hand
			}
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
			bool narrowed = narrow_villain_range_direct_blueprint(
				opp, would_be_allin ? BlueprintActionTranslation::Kind::AllIn
					: BlueprintActionTranslation::Kind::Raise,
				observed_whole_hand_total);
			if (!narrowed)
				narrow_villain_range_postflop(opp, would_be_allin ? (unsigned char)'n' : (unsigned char)2);
		}
		if (preflop)
			track_blueprint_action(opp, BlueprintActionTranslation::Kind::Raise, observed_whole_hand_total);
		g.stack[opp] = street_relative_raise_baseline(opp) - amount;
		g.last_raise_size = std::max(0, amount - prev_facing);
		g.blueprint_last_raise_size = g.last_raise_size;
		g.n_raises_this_street++;
		g.actions_this_street++;
		g.street_action_path.push_back(
			texassolver_bet_or_raise_token(prev_facing, opp_committed_before, amount));
	}
	else { // "call" or "check"
		if (preflop) { if (g.preflop_path_confident) narrow_villain_range_preflop('l'); }
		else if (!narrow_villain_range_direct_blueprint(
			opp, BlueprintActionTranslation::Kind::Call))
			narrow_villain_range_postflop(opp, 'l');
		if (preflop) track_blueprint_action(opp, BlueprintActionTranslation::Kind::Call);
		// See apply_own_action()'s matching comment / BUILD_NOTES.md section
		// 24: must use the raw 20000 whole-hand baseline here, not
		// g.stack_at_street_start[opp]-prev_facing, or the small blind's
		// preflop completing call/limp silently no-ops.
		int last_bigbet_before = std::max(20000 - g.stack[0], 20000 - g.stack[1]);
		g.stack[opp] = 20000 - last_bigbet_before;
		g.actions_this_street++;
		if (preflop && g.preflop_path_confident) g.preflop_action_path.push_back('l');
		g.street_action_path.push_back(texassolver_check_or_call_token(prev_facing));
	}
}

void getdecision(char* out_buf) {
	std::memset(out_buf, 0, 20);
	std::string action;
	if (g.betting_stage == 0) {
		action = resolve_preflop_decision();
	}
	else {
		action = resolve_direct_blueprint_decision();
		if (action.empty()) action = resolve_decision();
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
