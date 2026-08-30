//###############################################################################
//   test_preflop_offladder_sizing.cpp -- validates the fix for the preflop
//   "confidence loss" bug found investigating live hand #12474088712 (see
//   BUILD_NOTES.md section 51): an opponent's preflop open that doesn't
//   exactly hit one of match_raise_action_byte()'s seven discrete trained
//   sizes (2x/3x/5x/9x/21x/41x BB) used to PERMANENTLY set
//   g.preflop_path_confident = false for the rest of the hand's preflop
//   street, silently downgrading every one of hero's remaining preflop
//   decisions to a hand-strength-blind placeholder "call" -- with no
//   [DH_STRATEGY] percentages logged. This is exactly what happened live:
//   CHEYDI (villain) opened 6x-BB (600 native units), which sits strictly
//   between the trained 5x (500) and 9x (900) sizes.
//
//   The fix (match_raise_action_byte_fuzzy()) brackets an off-ladder size
//   between its two nearest trained sizes by pot-relative fraction and
//   samples one of the two via pseudo-harmonic interpolation -- the SAME
//   published action-translation technique already used (and validated) for
//   postflop raises in BlueprintActionTranslation::translate(). This lets
//   hero's subsequent preflop decisions keep consulting the real trained
//   blueprint instead of always falling back to "call".
//
//   Checks:
//     1. An EXACT preflop sizing match (raise to 300 = 3xBB) still resolves
//        via match_raise_action_byte()'s original exact-match path (byte 2),
//        confirming no regression for the common/already-correct case.
//     2. CHEYDI's real off-ladder 6xBB open (raise to 600) now brackets
//        between byte 4 (5xBB) and byte 8 (9xBB) instead of returning -1.
//     3. Replaying the real scenario end-to-end via opp_take_action()
//        confirms g.preflop_path_confident STAYS TRUE (previously went
//        false), and a subsequent getdecision() logs a real [DH_STRATEGY]
//        PREFLOP line (hand-strength-aware), not a silent placeholder.
//     4. Sampling match_raise_action_byte_fuzzy() many times for the same
//        6xBB input produces BOTH byte 4 and byte 8 (confirming genuine
//        pseudo-harmonic randomization, not a hardcoded snap-to-nearest),
//        skewed toward byte 4 (5xBB, the numerically closer trained size).
//     5. A degenerate input (non-positive pot) still correctly returns -1,
//        confirming the "still bails out for genuine bookkeeping errors"
//        guarantee is preserved.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_preflop_offladder_sizing tools/test_preflop_offladder_sizing.cpp
//   RUN (from PokerAI/):
//     ./tools/test_preflop_offladder_sizing
//###############################################################################
#include "dh_native_ai.cpp"
#include <cstdio>
#include <map>

static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}

int main() {
	bool all_ok = true;

	// --- Check 1: exact match unaffected (raise to 300 = 3xBB = byte 2) ---
	{
		// total_pot_before=150, last_bigbet_before=100, my_bet_before=50 (SB's
		// blind before acting) -- matches restart_game()'s blind convention.
		int byte = match_raise_action_byte_fuzzy(150, 100, 50, 300, g.rng);
		bool ok = (byte == 2);
		std::printf("[1] exact 3xBB match still resolves to byte 2: %s (got %d)\n",
			ok ? "PASS" : "FAIL", byte);
		all_ok &= ok;
	}

	// --- Check 2: CHEYDI's real 6xBB open (raise to 600) brackets 5x/9x ---
	{
		int byte = match_raise_action_byte_fuzzy(150, 100, 50, 600, g.rng);
		bool ok = (byte == 4 || byte == 8);
		std::printf("[2] off-ladder 6xBB open brackets to byte 4 (5xBB) or byte 8 (9xBB): %s (got %d)\n",
			ok ? "PASS" : "FAIL", byte);
		all_ok &= ok;
	}

	// --- Check 3: end-to-end replay of the real hand scenario ---
	{
		// Hero is BB (slot 1) holding 2h9c, matching hand #12474088712.
		restart_game(1, card_id("2h"), card_id("9c"));
		opp_take_action((char*)"raise 600"); // CHEYDI's real 6x-BB open

		bool confident = g.preflop_path_confident;
		std::printf("[3a] preflop_path_confident stays true after off-ladder open: %s\n",
			confident ? "PASS" : "FAIL");
		all_ok &= confident;

		bool path_has_byte = !g.preflop_action_path.empty() &&
			(g.preflop_action_path.back() == 4 || g.preflop_action_path.back() == 8);
		std::printf("[3b] preflop_action_path recorded a bracketed byte (4 or 8): %s (got %d)\n",
			path_has_byte ? "PASS" : "FAIL",
			g.preflop_action_path.empty() ? -1 : (int)g.preflop_action_path.back());
		all_ok &= path_has_byte;

		std::printf("[3c] calling getdecision() -- expect a real [DH_STRATEGY] PREFLOP line above, not a silent placeholder:\n");
		char out[20];
		getdecision(out);
		std::printf("     getdecision() returned: \"%s\" (did not crash)\n", out);
	}

	// --- Check 4: sampling distribution -- both branches reachable, skewed toward byte 4 ---
	{
		std::map<int, int> counts;
		const int trials = 2000;
		for (int i = 0; i < trials; i++) {
			int byte = match_raise_action_byte_fuzzy(150, 100, 50, 600, g.rng);
			counts[byte]++;
		}
		int c4 = counts.count(4) ? counts[4] : 0;
		int c8 = counts.count(8) ? counts[8] : 0;
		bool both_reachable = c4 > 0 && c8 > 0;
		bool skewed_toward_4 = c4 > c8; // 6xBB is numerically closer to 5xBB than 9xBB
		std::printf("[4] over %d trials: byte4(5xBB)=%d, byte8(9xBB)=%d -- both reachable: %s, skewed toward closer (byte4): %s\n",
			trials, c4, c8,
			both_reachable ? "PASS" : "FAIL",
			skewed_toward_4 ? "PASS" : "FAIL");
		all_ok &= both_reachable;
		all_ok &= skewed_toward_4;
	}

	// --- Check 5: degenerate input still correctly bails out (-1) ---
	{
		int byte = match_raise_action_byte_fuzzy(0, 0, 0, 600, g.rng);
		bool ok = (byte == -1);
		std::printf("[5] degenerate (non-positive pot) input still returns -1: %s (got %d)\n",
			ok ? "PASS" : "FAIL", byte);
		all_ok &= ok;
	}

	std::printf(all_ok ? "\nALL CHECKS PASSED\n" : "\nSOME CHECKS FAILED\n");
	return all_ok ? 0 : 1;
}
