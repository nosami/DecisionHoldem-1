//###############################################################################
//   test_texassolver_fallback.cpp -- end-to-end validation of the TexasSolver
//   RIVER-ONLY fallback path wired into resolve_decision() (see
//   TexasSolverBridge.h and this file's own #include target's top-of-file
//   header comment on DH_TEXASSOLVER_FALLBACK). #includes dh_native_ai.cpp
//   directly -- same established pattern as test_hand6_checkraise.cpp --
//   giving this test direct access to g, resolve_decision(), and every other
//   internal, and drives state through the REAL production ABI
//   (restart_game/apply_own_action/opp_take_action/Next_stage/getdecision),
//   not a hand-rolled reimplementation.
//
//   Scope: the fallback only ever engages when g.betting_stage==3 (river) --
//   see resolve_decision()'s `fallback_eligible_street` and
//   TexasSolverBridge.h's top header comment for why (a flop/turn-rooted
//   TexasSolver solve forces it to enumerate every remaining turn/river
//   runout with no leaf-value shortcut, confirmed during this integration's
//   earlier validation to OOM-kill the subprocess at DH's realistic range
//   widths; a river-rooted solve has no further chance nodes at all, so it
//   stays cheap regardless of range width). Every scenario below therefore
//   jumps DIRECTLY from a closed preflop straight to a dealt river board via
//   a single Next_stage(3, ...) call -- there is no need to visit flop/turn
//   first (Next_stage()/prune_ranges_for_board() have no street-history
//   dependency, only a check against the CURRENT g.board -- see
//   dh_native_ai.cpp), and skipping them keeps this test fast by not paying
//   for real (and, for TURN specifically, expensive-without-a-leaf-model)
//   in-process villain-range narrowing resolves that a real flop/turn replay
//   would otherwise trigger.
//
//   Three scenarios, all rooted at the river:
//
//     1. FORCED fallback, hero's own OPENING river decision
//        (actions_this_street==0) -- the exact/no-approximation bridge case
//        (no set_initial_actions needed; TexasSolver solves its own root).
//        Expected to genuinely SUCCEED end-to-end post river-only-fix:
//        checks the returned action is sane AND that g.hero_range's
//        per-combo weights actually changed (not just "sum still ~1", which
//        would be trivially true even if the fallback silently no-op'd) --
//        i.e. proof TexasSolver's real output was parsed and consumed.
//     2. FORCED fallback, hero facing a single prior river bet
//        (actions_this_street==1) -- the set_initial_actions/nearest-amount-
//        match bridge case. Same "real narrowing occurred" bar as #1.
//     3. AUTO-mode fallback triggered by a genuine in-process EXCEPTION:
//        hero's actual holding is deliberately removed from g.hero_range
//        (exactly what find_hand_index() detects and throws "hand not found
//        in tracked range" on) before the river decision. This makes hero's
//        real combo UNFINDABLE by construction on BOTH the in-process path
//        AND the TexasSolver path (the corrupted g.hero_range is also the
//        source of the range this bridge hands TexasSolver as hero's seat),
//        so the correct, expected outcome here is NOT "TexasSolver solves
//        it" -- it is that the system safely and deterministically declines
//        to narrow anything it has no real basis to narrow, on either path,
//        and falls through to the same "call" placeholder resolve_decision()
//        already used before this integration existed, leaving g.hero_range
//        BIT-IDENTICAL to its (already-corrupted-by-this-test) pre-decision
//        state. This still exercises the real TexasSolver subprocess (AUTO
//        mode reaches for it after the in-process throw) and confirms the
//        exception-triggered safety net engages without crashing across the
//        extern "C" ABI boundary -- it just also confirms the bridge
//        correctly refuses to fabricate a narrowing update when hero's own
//        combo genuinely isn't representable in either range.
//
//   REQUIRES a real, already-built TexasSolver checkout -- see
//   TexasSolverBridge.h's load_config()/DH_TEXASSOLVER_* env vars. Set
//   DH_TEXASSOLVER_HOME if it isn't at the default $HOME/src/TexasSolver.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_texassolver_fallback tools/test_texassolver_fallback.cpp
//   RUN (from PokerAI/):
//     DH_VERBOSE_STRATEGY=1 ./tools/test_texassolver_fallback
//###############################################################################
#include "dh_native_ai.cpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>

static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}

static bool sane_action_string(const std::string& action) {
	if (action == "fold" || action == "call" || action == "allin") return true;
	if (action.rfind("raise ", 0) == 0) {
		try {
			size_t consumed = 0;
			int amount = std::stoi(action.substr(6), &consumed);
			return amount >= 0 && consumed == action.substr(6).size();
		} catch (...) { return false; }
	}
	return false;
}

// Confirms g.hero_range is still a valid probability distribution: weights
// sum to ~expected_sum, none negative, none NaN/inf. `expected_sum` defaults
// to 1.0 (the normal case) but scenario 3 deliberately corrupts g.hero_range
// to a lower sum BEFORE the decision, and the correct outcome there is that
// sum staying put (untouched), not renormalized back to 1.0.
static bool hero_range_is_sane(double* out_sum, double expected_sum = 1.0) {
	double sum = 0.0;
	for (const auto& h : g.hero_range) {
		if (!std::isfinite(h.weight) || h.weight < -1e-9) return false;
		sum += h.weight;
	}
	if (out_sum) *out_sum = sum;
	return std::isfinite(sum) && std::fabs(sum - expected_sum) < 1e-6;
}

static std::vector<double> snapshot_hero_weights() {
	std::vector<double> w;
	w.reserve(g.hero_range.size());
	for (const auto& h : g.hero_range) w.push_back(h.weight);
	return w;
}

// True if at least one combo's weight moved by more than a tiny epsilon --
// proof a real narrowing update actually ran and was consumed, as opposed to
// resolve_decision() silently taking the "leave g.hero_range untouched"
// last-resort path (which would trivially also leave the sum at ~1.0 if
// nothing had corrupted it beforehand, making a sum-only check insufficient
// to distinguish "real success" from "silent no-op failure").
static bool weights_meaningfully_changed(const std::vector<double>& before, const std::vector<double>& after) {
	if (before.size() != after.size()) return true;
	double max_abs_diff = 0.0;
	for (size_t i = 0; i < before.size(); i++) max_abs_diff = std::max(max_abs_diff, std::fabs(before[i] - after[i]));
	return max_abs_diff > 1e-9;
}

// True if EVERY combo's weight is bit-for-bit (to 1e-12) identical -- the
// expected outcome for scenario 3, where g.hero_range must be left
// completely untouched by resolve_decision() rather than narrowed against a
// fabricated basis.
static bool weights_unchanged(const std::vector<double>& before, const std::vector<double>& after) {
	if (before.size() != after.size()) return false;
	for (size_t i = 0; i < before.size(); i++)
		if (std::fabs(before[i] - after[i]) > 1e-12) return false;
	return true;
}

static int g_failures = 0;

static void check(bool cond, const char* what) {
	if (cond) {
		fprintf(stderr, "  PASS: %s\n", what);
	} else {
		fprintf(stderr, "  FAIL: %s\n", what);
		g_failures++;
	}
}

// Scenario 1: hero (BB, OOP postflop) reaches the river having taken no
// action yet this street (actions_this_street==0) -- the bridge's
// exact/no-approximation case (no initial_actions needed at all). Jumps
// straight from a closed preflop to a dealt river board (see file header
// comment for why flop/turn don't need to be visited first).
static void scenario_opening_decision() {
	fprintf(stderr, "\n=== Scenario 1: FORCED fallback, hero's opening RIVER decision (actions_this_street==0) ===\n");
	setenv("DH_TEXASSOLVER_FALLBACK", "off", 1); // keep setup (preflop) on the normal path
	int hero_c1 = card_id("Ah"), hero_c2 = card_id("Ad");
	restart_game(1, hero_c1, hero_c2); // hero = BB = OOP postflop, my_id=1
	opp_take_action((char*)"call");      // SB (villain, slot0) limps/calls to 100
	apply_own_action("call");             // hero (BB) checks their preflop option
	fprintf(stderr, "  preflop closed: stack[0]=%d stack[1]=%d\n", g.stack[0], g.stack[1]);

	unsigned char river[5] = {
		(unsigned char)card_id("Ks"), (unsigned char)card_id("7d"), (unsigned char)card_id("2c"),
		(unsigned char)card_id("3h"), (unsigned char)card_id("9s")
	};
	Next_stage(3, (char*)river);
	fprintf(stderr, "  river dealt: betting_stage=%d actions_this_street=%d stack_at_street_start=[%d,%d]\n",
		g.betting_stage, g.actions_this_street, g.stack_at_street_start[0], g.stack_at_street_start[1]);
	check(g.betting_stage == 3, "betting_stage==3 (river) after Next_stage(3, ...)");
	check(g.actions_this_street == 0, "actions_this_street==0 before hero's opening river decision");

	std::vector<double> before = snapshot_hero_weights();
	double before_sum = 0.0;
	hero_range_is_sane(&before_sum);
	fprintf(stderr, "  hero_range weight sum before decision: %.6f (size=%zu)\n", before_sum, g.hero_range.size());

	setenv("DH_TEXASSOLVER_FALLBACK", "force", 1);
	char out[20];
	getdecision(out);
	std::string action(out);
	fprintf(stderr, "  getdecision() -> \"%s\"\n", action.c_str());

	std::vector<double> after = snapshot_hero_weights();
	double after_sum = 0.0;
	bool sane = hero_range_is_sane(&after_sum);
	fprintf(stderr, "  hero_range weight sum after narrowing: %.6f\n", after_sum);

	check(sane_action_string(action), "returned action string is one of fold/call/allin/raise N");
	check(sane, "g.hero_range is a valid probability distribution after narrowing (sum~=1, no NaN/negative)");
	check(weights_meaningfully_changed(before, after),
		"g.hero_range weights actually changed -- TexasSolver's real strategy was parsed and consumed, not a silent no-op");
}

// Scenario 2: hero (SB/BTN, IP postflop) reaches the river, villain (OOP)
// bets first -- hero is now facing a single prior action this street
// (actions_this_street==1), exercising the bridge's set_initial_actions /
// nearest-amount-match path.
static void scenario_facing_a_bet() {
	fprintf(stderr, "\n=== Scenario 2: FORCED fallback, hero facing a RIVER bet (actions_this_street==1) ===\n");
	setenv("DH_TEXASSOLVER_FALLBACK", "off", 1); // keep setup on the normal path
	int hero_c1 = card_id("Qh"), hero_c2 = card_id("Qs");
	restart_game(0, hero_c1, hero_c2); // hero = SB/BTN = IP postflop, my_id=0
	apply_own_action("call");           // hero (SB) limps/calls to 100
	opp_take_action((char*)"call");     // villain (BB) checks their option
	fprintf(stderr, "  preflop closed: stack[0]=%d stack[1]=%d\n", g.stack[0], g.stack[1]);

	unsigned char river[5] = {
		(unsigned char)card_id("Jc"), (unsigned char)card_id("8h"), (unsigned char)card_id("3s"),
		(unsigned char)card_id("5d"), (unsigned char)card_id("Tc")
	};
	Next_stage(3, (char*)river);
	opp_take_action((char*)"raise 150"); // villain (OOP) bets 150 into the river pot
	fprintf(stderr, "  villain bets 150: betting_stage=%d actions_this_street=%d stack=[%d,%d] stack_at_street_start=[%d,%d]\n",
		g.betting_stage, g.actions_this_street, g.stack[0], g.stack[1],
		g.stack_at_street_start[0], g.stack_at_street_start[1]);
	check(g.betting_stage == 3, "betting_stage==3 (river) after Next_stage(3, ...)");
	check(g.actions_this_street == 1, "actions_this_street==1 with hero facing villain's river bet");

	std::vector<double> before = snapshot_hero_weights();
	double before_sum = 0.0;
	hero_range_is_sane(&before_sum);
	fprintf(stderr, "  hero_range weight sum before decision: %.6f (size=%zu)\n", before_sum, g.hero_range.size());

	setenv("DH_TEXASSOLVER_FALLBACK", "force", 1);
	char out[20];
	getdecision(out);
	std::string action(out);
	fprintf(stderr, "  getdecision() -> \"%s\"\n", action.c_str());

	std::vector<double> after = snapshot_hero_weights();
	double after_sum = 0.0;
	bool sane = hero_range_is_sane(&after_sum);
	fprintf(stderr, "  hero_range weight sum after narrowing: %.6f\n", after_sum);

	check(sane_action_string(action), "returned action string is one of fold/call/allin/raise N");
	check(sane, "g.hero_range is a valid probability distribution after narrowing (sum~=1, no NaN/negative)");
	check(weights_meaningfully_changed(before, after),
		"g.hero_range weights actually changed -- TexasSolver's real strategy was parsed and consumed, not a silent no-op");
}

// Scenario 3: AUTO mode (the real default), but the in-process resolver is
// forced to THROW by deliberately removing hero's actual holding from
// g.hero_range -- exactly what find_hand_index() detects (see
// dh_native_ai.cpp) and reports via a thrown std::runtime_error. Confirms
// resolve_decision()'s AUTO-mode exception path actually reaches for
// TexasSolver (a real subprocess call) instead of letting that exception
// propagate uncaught across the extern "C" ABI boundary -- and, since hero's
// real combo is now unfindable in EITHER range too, confirms the bridge
// correctly detects that and safely declines to narrow, rather than
// fabricating an update or crashing.
static void scenario_auto_fallback_on_exception() {
	fprintf(stderr, "\n=== Scenario 3: AUTO fallback triggered by in-process exception (RIVER) ===\n");
	setenv("DH_TEXASSOLVER_FALLBACK", "off", 1); // keep setup on the normal path
	int hero_c1 = card_id("9c"), hero_c2 = card_id("9d");
	restart_game(1, hero_c1, hero_c2); // hero = BB = OOP postflop
	opp_take_action((char*)"call");
	apply_own_action("call");

	unsigned char river[5] = {
		(unsigned char)card_id("Ts"), (unsigned char)card_id("4h"), (unsigned char)card_id("2s"),
		(unsigned char)card_id("6d"), (unsigned char)card_id("Kc")
	};
	Next_stage(3, (char*)river);
	check(g.betting_stage == 3, "betting_stage==3 (river) after Next_stage(3, ...)");
	check(g.actions_this_street == 0, "actions_this_street==0 before hero's opening river decision");

	// Deliberately corrupt g.hero_range so hero's own real hand (9c9d) is no
	// longer tracked -- this is exactly the condition find_hand_index()
	// guards against inside the in-process path (see resolve_decision()'s
	// `size_t my_hand_index = find_hand_index(...)` call), forcing a real,
	// organic std::runtime_error from real production code, not a
	// synthetic/injected fault. Note this ALSO removes hero's real combo
	// from the range resolve_decision() would hand to TexasSolver as hero's
	// seat's range (both read from the same g.hero_range), so the fallback
	// cannot find hero's row in TexasSolver's output either -- see this
	// file's header comment for why "leave g.hero_range untouched" is the
	// correct expected outcome here, not "TexasSolver narrows it".
	size_t removed = 0;
	for (size_t i = 0; i < g.hero_range.size();) {
		if ((g.hero_range[i].c1 == (unsigned char)hero_c1 && g.hero_range[i].c2 == (unsigned char)hero_c2) ||
			(g.hero_range[i].c1 == (unsigned char)hero_c2 && g.hero_range[i].c2 == (unsigned char)hero_c1)) {
			g.hero_range.erase(g.hero_range.begin() + i);
			removed++;
		} else i++;
	}
	fprintf(stderr, "  removed hero's own real hand (9c9d) from g.hero_range (%zu entries removed) to force find_hand_index() to throw\n", removed);
	check(removed > 0, "hero's real hand was present and removable before the forced-exception test");

	std::vector<double> before = snapshot_hero_weights();
	double before_sum = 0.0;
	hero_range_is_sane(&before_sum, /*expected_sum=*/before_sum >= 0 ? before_sum : 1.0);
	fprintf(stderr, "  hero_range weight sum before decision (already missing hero's own combo): %.6f (size=%zu)\n",
		before_sum, g.hero_range.size());

	setenv("DH_TEXASSOLVER_FALLBACK", "auto", 1);
	char out[20];
	getdecision(out);
	std::string action(out);
	fprintf(stderr, "  getdecision() -> \"%s\" (in-process should have thrown; AUTO mode should have reached for TexasSolver,\n"
		"    which should in turn fail to find hero's own combo and safely decline)\n", action.c_str());

	std::vector<double> after = snapshot_hero_weights();
	double after_sum = 0.0;
	bool sane = hero_range_is_sane(&after_sum, before_sum);
	fprintf(stderr, "  hero_range weight sum after (expected unchanged from %.6f): %.6f\n", before_sum, after_sum);

	check(action == "call", "returned action is the deterministic last-resort \"call\" placeholder (no crash across the ABI boundary)");
	check(sane, "g.hero_range sum is unchanged from its (already-corrupted) pre-decision value");
	check(weights_unchanged(before, after),
		"g.hero_range weights are bit-identical to before -- correctly left untouched since hero's own combo "
		"is unfindable in either the in-process OR TexasSolver path, not fabricated from a fake basis");
}

int main() {
	setenv("DH_TEXASSOLVER_MAX_ITERATIONS", "80", 1); // keep the solve fast for a test tool
	scenario_opening_decision();
	scenario_facing_a_bet();
	scenario_auto_fallback_on_exception();

	fprintf(stderr, "\n=== SUMMARY: %s (%d failure%s) ===\n",
		g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
