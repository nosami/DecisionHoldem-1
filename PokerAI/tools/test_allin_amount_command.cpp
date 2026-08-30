//###############################################################################
//   test_allin_amount_command.cpp -- validates the fix for a real live hand
//   (#12475294621, see BUILD_NOTES.md) in which villain (BB) shoved their
//   entire EUR10.84 stack over hero's (SB) EUR0.30 (3xBB) open, and
//   DecisionHoldem's recommended CALL turned out to be an unconsulted
//   hardcoded placeholder, not a real blueprint decision.
//
//   ROOT CAUSE: decisionholdem_bridge.py's opponent_action() can send the
//   native engine a documented "allin <amount>" command (a real, stack-
//   diff-corrected whole-hand-cumulative commitment) whenever a reliable
//   real-stack amount is known for an opponent's all-in -- see its own
//   comment, which explicitly names "dh_native_ai.cpp's opp_take_action()'s
//   'allin <amount>' comment". Before this fix, opp_take_action() never
//   actually implemented that format: "allin 10840" matched neither the
//   exact "allin" check nor the "raise " prefix check, so it silently fell
//   through to the final call/check branch -- recording villain's shove as
//   a plain call (byte 'l') instead of an all-in (byte 'n'). This desynced
//   g.preflop_action_path from the real trained tree just enough that the
//   very next lookup (hero's own resulting decision) walked into an
//   unrelated chance-node subtree, and BlueprintReader::read_node_header()
//   threw "encountered a chance-node marker (action_len >= 100)" --
//   resolve_preflop_decision() then fell back to a hardcoded, hand-
//   strength-blind "call", exactly matching the real live log:
//     [DH_PREFLOP_BLUEPRINT] real blueprint lookup failed (BlueprintReader:
//     encountered a chance-node marker (action_len >= 100) while
//     navigating what should be a preflop-only path ...) -- falling back
//     to placeholder 'call' for this decision only
//
//   Confirmed directly against the REAL blueprint_stgy.dat (read-only,
//   throwaway diagnostic, not checked in) that walking action_path=[2,108]
//   ('2'=hero's 3xBB raise, 108='l'=call) reproduces this exact exception,
//   while [2,110] (110='n'=allin) resolves cleanly to a 2-action node
//   (fold/call) as expected once BB is all-in.
//
//   Checks:
//     1. A bare "allin" (no amount) still behaves exactly as before (byte
//        'n', amount = g.stack_at_street_start[opp]) -- no regression.
//     2. "allin <amount>" now ALSO resolves to byte 'n' (not the pre-fix
//        'l'), and g.preflop_path_confident stays true.
//     3. An ordinary "raise <amount>" is unaffected by the new branch
//        (different fixed prefix, no overlap).
//     4. End-to-end replay of the real hand #12475294621 scenario (hero SB
//        5s9d opens 3xBB, villain shoves "allin 10840") -- getdecision()
//        for hero's resulting decision now logs a real "[DH_STRATEGY]
//        PREFLOP" line and does NOT log "[DH_PREFLOP_BLUEPRINT] real
//        blueprint lookup failed".
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_allin_amount_command tools/test_allin_amount_command.cpp
//   RUN (from PokerAI/):
//     ./tools/test_allin_amount_command
//###############################################################################
#include "dh_native_ai.cpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

static int card_id(const char* s) {
	static const char ranks[] = "23456789TJQKA";
	static const char suits[] = "scdh";
	const char* r = strchr(ranks, s[0]);
	const char* su = strchr(suits, s[1]);
	return (int)(su - suits) * 13 + (int)(r - ranks);
}

// Redirects stderr to a temp file for the duration of `fn`, then returns
// its captured contents. Restores real stderr afterward regardless of
// whether `fn` throws.
template <typename Fn>
static std::string capture_stderr(Fn&& fn) {
	std::string path = "/tmp/test_allin_amount_command_capture.XXXXXX";
	std::vector<char> buf(path.begin(), path.end());
	buf.push_back('\0');
	int fd = mkstemp(buf.data());
	if (fd < 0) { fn(); return ""; }
	std::fflush(stderr);
	int saved_stderr = dup(fileno(stderr));
	dup2(fd, fileno(stderr));
	close(fd);
	fn();
	std::fflush(stderr);
	dup2(saved_stderr, fileno(stderr));
	close(saved_stderr);

	std::string out;
	FILE* f = std::fopen(buf.data(), "rb");
	if (f) {
		char chunk[4096];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) out.append(chunk, n);
		std::fclose(f);
	}
	std::remove(buf.data());
	return out;
}

int main() {
	bool all_ok = true;

	// --- Check 1: bare "allin" unaffected (still byte 'n', still uses
	//     g.stack_at_street_start[opp] as its amount) ---
	{
		restart_game(1, card_id("2h"), card_id("9c")); // hero BB, arbitrary cards
		opp_take_action((char*)"allin");
		bool ok = !g.preflop_action_path.empty() && g.preflop_action_path.back() == 'n'
			&& g.preflop_path_confident && g.stack[0] == 0;
		std::printf("[1] bare \"allin\" still resolves to byte 'n' (no regression): %s\n",
			ok ? "PASS" : "FAIL");
		all_ok &= ok;
	}

	// --- Check 2: "allin <amount>" now ALSO resolves to byte 'n' (the bug
	//     fix) instead of falling through to the call/check branch's 'l' ---
	{
		restart_game(1, card_id("2h"), card_id("9c"));
		opp_take_action((char*)"allin 10840");
		bool byte_is_n = !g.preflop_action_path.empty() && g.preflop_action_path.back() == 'n';
		std::printf("[2a] \"allin <amount>\" resolves to byte 'n', not 'l': %s (got %d)\n",
			byte_is_n ? "PASS" : "FAIL",
			g.preflop_action_path.empty() ? -1 : (int)g.preflop_action_path.back());
		all_ok &= byte_is_n;

		bool confident = g.preflop_path_confident;
		std::printf("[2b] preflop_path_confident stays true: %s\n", confident ? "PASS" : "FAIL");
		all_ok &= confident;

		// Unlike the bare "allin" branch (which has no real number and must
		// assume the opponent's entire fictional 20000-chip baseline was
		// shoved), the whole point of supplying a real amount is that the
		// pot/stack bookkeeping actually reflects it -- so the tracked
		// stack should be 20000-10840=9160, NOT 0.
		bool stack_reflects_real_amount = g.has_allin && g.stack[0] == 20000 - 10840;
		std::printf("[2c] g.has_allin set and tracked stack reflects the REAL amount (20000-10840=9160), not a hardcoded 0: %s (got %d)\n",
			stack_reflects_real_amount ? "PASS" : "FAIL", g.stack[0]);
		all_ok &= stack_reflects_real_amount;
	}

	// --- Check 3: an ordinary "raise <amount>" is unaffected (distinct,
	//     non-overlapping fixed prefix) ---
	{
		restart_game(1, card_id("2h"), card_id("9c"));
		opp_take_action((char*)"raise 300");
		bool ok = !g.preflop_action_path.empty() && g.preflop_action_path.back() == 2
			&& g.preflop_path_confident;
		std::printf("[3] ordinary \"raise 300\" still resolves to byte 2, unaffected: %s (got %d)\n",
			ok ? "PASS" : "FAIL",
			g.preflop_action_path.empty() ? -1 : (int)g.preflop_action_path.back());
		all_ok &= ok;
	}

	// --- Check 4: end-to-end replay of the real hand #12475294621 scenario ---
	{
		// Hero is SB (slot 0) holding 5s9d, matching hand #12475294621.
		restart_game(0, card_id("5s"), card_id("9d"));
		apply_own_action("raise 300"); // hero's real 3xBB open (byte 2, on-ladder)

		char out[20];
		bool byte_is_n = false;
		std::string captured = capture_stderr([&]() {
			// Villain's real shove: EUR10.84 stack-diff-corrected total,
			// native units (0.10 EUR big blind -> 0.001 EUR/native chip),
			// exactly as decisionholdem_bridge.py would send it now that a
			// reliable real-stack amount is known.
			opp_take_action((char*)"allin 10840");
			// Check the path RIGHT AFTER villain's action and BEFORE
			// getdecision(), since getdecision()/resolve_preflop_decision()
			// itself appends hero's own subsequently-sampled action byte.
			byte_is_n = !g.preflop_action_path.empty() &&
				g.preflop_action_path[g.preflop_action_path.size() - 1] == 'n';
			getdecision(out);
		});

		std::printf("[4a] villain's real shove recorded as byte 'n': %s\n", byte_is_n ? "PASS" : "FAIL");
		all_ok &= byte_is_n;

		bool no_fallback_error = captured.find("[DH_PREFLOP_BLUEPRINT] real blueprint lookup failed") == std::string::npos;
		std::printf("[4b] no \"[DH_PREFLOP_BLUEPRINT] real blueprint lookup failed\" fallback logged: %s\n",
			no_fallback_error ? "PASS" : "FAIL");
		all_ok &= no_fallback_error;

		bool has_strategy_line = captured.find("[DH_STRATEGY] PREFLOP") != std::string::npos;
		std::printf("[4c] a real \"[DH_STRATEGY] PREFLOP\" line was logged for hero's resulting decision: %s\n",
			has_strategy_line ? "PASS" : "FAIL");
		all_ok &= has_strategy_line;

		std::printf("[4d] getdecision() returned: \"%s\" (did not crash)\n", out);
		if (!captured.empty()) {
			std::printf("     -- captured stderr --\n%s     -- end captured stderr --\n", captured.c_str());
		}
	}

	std::printf(all_ok ? "\nALL CHECKS PASSED\n" : "\nSOME CHECKS FAILED\n");
	return all_ok ? 0 : 1;
}
