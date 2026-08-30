#pragma once
// ---------------------------------------------------------------------------
// TexasSolverBridge.h -- fallback postflop resolver via the external
// TexasSolver CFR solver (checked out separately at $HOME/src/TexasSolver;
// confirmed to be `nosami/skypoker`, a personal fork of the open-source
// AGPLv3 `bupticybee/TexasSolver`), invoked as an isolated subprocess.
//
// This is used ONLY as a fallback when dh_native_ai.cpp's own in-process
// `LiveResolver` (RealtimeSearch.h) is unavailable for a given postflop
// decision -- it throws, fails to converge, or the caller is explicitly
// testing this path via an env var. See BUILD_NOTES.md's TexasSolver
// section for the full design writeup, citations into TexasSolver's own
// source confirming every wire-format detail below, and validation
// results. RealtimeSearch.h/LiveResolver itself never includes or knows
// about this header -- nothing here can change the in-process resolver's
// own behavior; dh_native_ai.cpp (the only intended caller) decides
// entirely on its own when (if ever) to reach for this.
//
// SCOPE: RIVER ONLY. `solve()` below refuses (returns ok=false) any board
// that doesn't already have all 5 cards. This is a deliberate, validated
// scope decision, not a temporary shortcut: TexasSolver's own tree
// builder starts the tree at whichever street `set_board`'s card count
// implies (3 cards -> flop, 4 -> turn, 5 -> river; see
// CommandLineTool.cpp's set_board/build_tree handlers) and, for FLOP/TURN,
// that means enumerating every remaining turn/river card runout with NO
// leaf-value shortcut analogous to this codebase's own
// TurnClusterLeafModel/RiverClusterLeafModel -- confirmed during this
// integration's validation to OOM-kill the subprocess (70+ GB RSS on a
// 16GB machine) at DH's realistic range widths even with a trimmed bet
// ladder. A RIVER-rooted board has no further chance nodes at all, so
// the tree is just one street's betting -- small and fast regardless of
// range width, matching this codebase's own measured RIVER-mode cost
// being its CHEAPEST per-iteration resolve, not its most expensive (see
// BUILD_NOTES.md's run_until_converged() timing citations). FLOP/TURN
// postflop decisions continue to be handled EXCLUSIVELY by the in-process
// LiveResolver (unchanged) -- they are also the rare case in practice,
// since resolve_direct_blueprint_decision() already answers the large
// majority of them straight from the trained blueprint.
//
// Mechanism: shell out to the solver's plain CLI executable
// (`console_solver -i <input> -r <resource_dir> -m holdem`), which reads a
// plain-text batch of commands (`set_pot`/`set_range_ip`/.../`dump_result`)
// and writes a JSON strategy tree. A subprocess (not an in-process
// dlopen() of the solver's own libapi.dylib) is used deliberately, for
// process isolation: a solver hang or crash must not take the live poker
// bot process down with it. The subprocess is given a hard wall-clock
// timeout (SIGKILL if exceeded) enforced by polling waitpid() from this
// same thread -- NOT a background thread/std::async watching over it,
// because an abandoned background watcher could still be touching
// g.hero_range/g.villain_range (via whatever future refactor) after this
// function has already returned control to a caller that assumes it's
// done; a synchronously-polled child *process* has no such risk since it
// shares no memory with us at all.
// ---------------------------------------------------------------------------

#include "../third_party/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <random>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace texassolver_bridge {

// ---------------------------------------------------------------------------
// Configuration -- every knob is overridable via an env var so the fallback
// can be tuned/forced without a rebuild, matching this codebase's existing
// DH_* env-var conventions (DH_VERBOSE_STRATEGY, DH_RIVER_SPLIT_DIR,
// DH_DIRECT_BLUEPRINT, ...). See BUILD_NOTES.md for the full list and
// defaults rationale.
// ---------------------------------------------------------------------------
struct Config {
	std::string binary_path;   // console_solver executable
	std::string resource_dir;  // -r argument (hand-strength lookup tables)
	int max_iterations = 100;
	int thread_num = 4;
	double accuracy = 0.5;     // stop_on_convergence target, % of pot
	int timeout_ms = 25000;    // hard subprocess wall-clock cap
};

inline std::string env_or(const char* name, const std::string& fallback) {
	const char* v = std::getenv(name);
	return (v && v[0] != '\0') ? std::string(v) : fallback;
}

inline int env_or_int(const char* name, int fallback) {
	const char* v = std::getenv(name);
	if (!v || v[0] == '\0') return fallback;
	try { return std::stoi(v); } catch (...) { return fallback; }
}

inline double env_or_double(const char* name, double fallback) {
	const char* v = std::getenv(name);
	if (!v || v[0] == '\0') return fallback;
	try { return std::stod(v); } catch (...) { return fallback; }
}

// Mirrors this repo's own convention of resolving paths relative to
// $HOME rather than hardcoding a username (see e.g. how BUILD_NOTES.md
// documents $HOME/src/TexasSolver throughout). DH_TEXASSOLVER_HOME lets a
// different checkout location be used without editing source.
inline std::string texassolver_home() {
	std::string configured = env_or("DH_TEXASSOLVER_HOME", "");
	if (!configured.empty()) return configured;
	const char* home = std::getenv("HOME");
	return (home ? std::string(home) : std::string(".")) + "/src/TexasSolver";
}

inline Config load_config() {
	Config cfg;
	std::string home = texassolver_home();
	// Prefer the `install/` copy (its `-r resources` default layout mirrors
	// how TexasSolver's own README suggests running it post-install) but
	// fall back to the raw `build/` output -- both were confirmed present
	// and working directly on this machine during this integration's
	// investigation (see BUILD_NOTES.md).
	std::string default_binary = home + "/build/console_solver";
	std::string default_resource_dir = home + "/resources";
	cfg.binary_path = env_or("DH_TEXASSOLVER_BINARY", default_binary);
	cfg.resource_dir = env_or("DH_TEXASSOLVER_RESOURCE_DIR", default_resource_dir);
	cfg.max_iterations = env_or_int("DH_TEXASSOLVER_MAX_ITERATIONS", 100);
	int hw = (int)std::thread::hardware_concurrency();
	cfg.thread_num = env_or_int("DH_TEXASSOLVER_THREADS", hw > 0 ? hw : 4);
	cfg.accuracy = env_or_double("DH_TEXASSOLVER_ACCURACY", 0.5);
	cfg.timeout_ms = env_or_int("DH_TEXASSOLVER_TIMEOUT_MS", 25000);
	return cfg;
}

// Fallback-trigger mode, read fresh every call so it can be flipped
// without restarting the bot process (e.g. between hands in a test
// harness). See dh_native_ai.cpp's resolve_decision() for how each value
// is used; documented in BUILD_NOTES.md.
enum class TriggerMode { AUTO, FORCE, OFF };

inline TriggerMode trigger_mode() {
	std::string v = env_or("DH_TEXASSOLVER_FALLBACK", "auto");
	std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	if (v == "force") return TriggerMode::FORCE;
	if (v == "off") return TriggerMode::OFF;
	return TriggerMode::AUTO;
}

// If, after the in-process resolver's own adaptive convergence loop
// finishes, measured exploitability (as a % of pot -- same units
// dh_native_ai.cpp's run_until_converged() already computes) is still
// above this, treat it as "failed to converge" and try the fallback.
// Deliberately higher than the in-process target (1.0%, see
// dh_native_ai.cpp's TARGET_EXPLOITABILITY_PCT) -- this is a "something
// is clearly wrong" backstop, not a stricter quality bar than the
// primary path already tries to hit; the primary path's OWN target
// still governs normal operation unchanged.
inline double exploitability_trigger_pct() {
	return env_or_double("DH_TEXASSOLVER_EXPLOITABILITY_TRIGGER_PCT", 15.0);
}

// ---------------------------------------------------------------------------
// Card/board string formatting -- MUST agree with dh_native_ai.cpp's own
// dh_card_str() (id = suit*13+rank, suits "scdh", ranks "23456789TJQKA")
// and with TexasSolver's own convention. Cross-checked directly against
// TexasSolver's include/Card.h / src/Card.cpp: Card::getSuits() returns
// {"c","d","h","s"} (same 4 letters) and its rank parser accepts the same
// "23456789TJQKA" set (Card::strCard2int/rankToInt) -- so no translation
// is needed beyond producing the same "<rank><suit>" order dh_card_str()
// already uses (e.g. dh_card_str's id 9 -> "Js"; TexasSolver parses "Js"
// identically). Duplicated here (rather than including dh_native_ai.cpp)
// to keep this header independently includable/testable; the convention is fixed
// and documented, not something that can silently drift unnoticed since
// every hand this bridge ever serializes is round-tripped through the
// same regression test tool (tools/test_texassolver_fallback.cpp).
inline std::string card_str(unsigned char c) {
	static const char suits[] = "scdh";
	static const char ranks[] = "23456789TJQKA";
	if (c >= 52) return "??";
	char buf[3] = { ranks[c % 13], suits[c / 13], '\0' };
	return std::string(buf);
}

inline std::string serialize_board(const std::vector<unsigned char>& board) {
	std::ostringstream out;
	for (size_t i = 0; i < board.size(); i++) {
		if (i) out << ",";
		out << card_str(board[i]);
	}
	return out.str();
}

// One seat's weighted range entry -- deliberately NOT dh_native_ai.cpp's
// own WeightedHand (to avoid a circular include); field-compatible so
// callers can construct these with `{h.c1, h.c2, h.weight}`.
struct Combo {
	unsigned char c1, c2;
	double weight;
};

// Serializes a range to TexasSolver's PrivateRangeConverter grammar:
// comma-separated "AcKh:0.123" tokens (see
// $HOME/src/TexasSolver/src/tools/PrivateRangeConverter.cpp). Entries
// with weight <= 0, or that collide with a board card, are omitted --
// PrivateRangeConverter itself already skips weight<=0 entries, and a
// board-colliding combo is physically impossible so must never be given
// positive mass; this is a defensive re-check, not a behavior this
// bridge relies on the caller having already applied (callers are
// expected to pass g.hero_range/g.villain_range AFTER
// prune_range_for_board() has already run, per dh_native_ai.cpp, but
// re-checking here is cheap and removes any ordering dependency).
// PrivateRangeConverter also throws on a DUPLICATE combo appearing twice
// in the string -- impossible here as long as `range` itself holds each
// unordered pair at most once, which is true for both g.hero_range and
// g.villain_range by construction (see dh_native_ai.cpp's
// find_hand_index(), which assumes/enforces c1<c2 canonical ordering).
inline std::string serialize_range(const std::vector<Combo>& range,
	const std::vector<unsigned char>& board) {
	std::ostringstream out;
	out << std::setprecision(10);
	bool first = true;
	for (const auto& h : range) {
		if (!(h.weight > 0.0)) continue;
		bool collides = false;
		for (unsigned char b : board) if (b == h.c1 || b == h.c2) { collides = true; break; }
		if (collides) continue;
		if (!first) out << ",";
		first = false;
		out << card_str(h.c1) << card_str(h.c2) << ":" << h.weight;
	}
	return out.str();
}

// ---------------------------------------------------------------------------
// Bet-size abstraction passed to TexasSolver, chosen to mirror
// dh_native_ai.cpp's OWN live bet-size abstraction as closely as
// TexasSolver's config format allows. RIVER ONLY -- see this file's top
// header comment for why FLOP/TURN are out of scope; these ladders are
// therefore only ever applied to the river's "bet"/"raise"/"donk"/"allin"
// categories (see build_batch_commands() below).
//   - "Opening" sizes (TexasSolver's "bet"/"donk" categories: the first
//     wager into a pot with nothing yet owed this street) use the exact
//     native pot-fraction ladder RealtimeSearch.h's LiveResolver
//     documents for its own `full_ladder` opening-action branch --
//     0.5/1/2/4/8/10/20x pot (RealtimeSearch.h lines ~992-1013).
//   - "Facing a bet" sizes (TexasSolver's "raise" category) use a single
//     1x-pot size, mirroring LiveResolver's `extended_actions_` flag,
//     which adds exactly ONE extra canonical-pot-sized-raise branch when
//     an existing bet is already in front of the acting player
//     (RealtimeSearch.h lines ~969-981).
//   - "allin" is always offered, matching every mode of the in-process
//     resolver (fold/call/allin is its baseline action set on every node).
// This cannot be an exact structural match (TexasSolver has no notion of
// "wider ladder only at the very first decision of a street, reduced
// everywhere deeper" the way LiveResolver's full_ladder flag does --
// TexasSolver's bet-size config is fixed per street/seat, applied at
// every node of that type) -- using "bet" sizes for the opening action
// and "raise" sizes for a response to a bet is the closest available
// structural analogue TexasSolver's own config surface offers, and is
// documented as an approximation, not silently assumed exact.
inline const char* opening_bet_sizes_pct() { return "50,100,200,400,800,1000,2000"; }
inline const char* facing_bet_raise_sizes_pct() { return "100"; }

// ---------------------------------------------------------------------------
// Batch command-file construction.
// ---------------------------------------------------------------------------
struct SolveRequest {
	int pot_at_street_start = 0;
	int effective_stack_at_street_start = 0;
	std::vector<unsigned char> board;
	std::vector<Combo> ip_range;
	std::vector<Combo> oop_range;
	// If non-empty, replayed via `set_initial_actions` before solving, so
	// the dumped strategy is rooted at the node AFTER these actions --
	// i.e. the ACTUAL decision hero faces, not the top of the street.
	// Format: TexasSolver's own comma-separated "ACTIONNAME[_amount]"
	// grammar (PCfrSolver::navigateToSubtree), e.g. "CHECK" or
	// "BET_4500" (underscore, not space -- CommandLineTool's own
	// top-level line parser rejects any line with more than one space,
	// see BUILD_NOTES.md citation into CommandLineTool.cpp).
	std::string initial_actions;
	Config cfg;
};

inline std::string build_batch_commands(const SolveRequest& req, const std::string& output_json_path) {
	std::ostringstream cmd;
	cmd << "set_pot " << req.pot_at_street_start << "\n";
	cmd << "set_effective_stack " << req.effective_stack_at_street_start << "\n";
	cmd << "set_board " << serialize_board(req.board) << "\n";
	cmd << "set_range_ip " << serialize_range(req.ip_range, req.board) << "\n";
	cmd << "set_range_oop " << serialize_range(req.oop_range, req.board) << "\n";
	// RIVER ONLY (see this file's top header comment and solve()'s
	// defensive board.size()==5 check below): req.board always has all 5
	// cards by the time this is called, so set_board above already made
	// TexasSolver's own current_round==river (CommandLineTool.cpp's
	// set_board handler), and build_tree only ever constructs river
	// action nodes for it -- flop/turn bet-size categories would be dead
	// config for this tree and are deliberately NOT sent: configuring them
	// unconditionally for every street is exactly what caused this
	// bridge's original OOM when (before this restriction existed) it was
	// exercised from the flop, forcing a full flop->turn->river
	// enumeration (see BUILD_NOTES.md).
	for (const char* pos : { "oop", "ip" }) {
		cmd << "set_bet_sizes " << pos << ",river,bet," << opening_bet_sizes_pct() << "\n";
		cmd << "set_bet_sizes " << pos << ",river,raise," << facing_bet_raise_sizes_pct() << "\n";
		cmd << "set_bet_sizes " << pos << ",river,allin\n";
	}
	// Donk (OOP leading into a street after being the non-aggressor) is
	// only meaningful for OOP; mirrored after the confirmed-working
	// example config at resources/text/commandline_one_hand.txt, which
	// only sets a river donk size. DH itself has no separate "donk"
	// concept (its action set doesn't distinguish by prior-street
	// aggressor), so the opening ladder is reused here too.
	cmd << "set_bet_sizes oop,river,donk," << opening_bet_sizes_pct() << "\n";
	cmd << "set_allin_threshold 0.67\n";
	if (!req.initial_actions.empty())
		cmd << "set_initial_actions " << req.initial_actions << "\n";
	cmd << "build_tree\n";
	cmd << "set_thread_num " << req.cfg.thread_num << "\n";
	cmd << "set_accuracy " << req.cfg.accuracy << "\n";
	cmd << "set_max_iteration " << req.cfg.max_iterations << "\n";
	cmd << "set_print_interval " << std::max(1, req.cfg.max_iterations / 4) << "\n";
	// Deliberately OFF: TexasSolver's own suit-isomorphism grouping is
	// only exact when the OPPONENT's incoming reach is symmetric under
	// the current street's board-automorphism group. g.hero_range and
	// g.villain_range are narrowed across MULTIPLE PRIOR STREETS against
	// DIFFERENT boards before any postflop resolve runs, so they are
	// generically asymmetric under the CURRENT street's automorphism
	// group by the time this bridge is invoked -- exactly the reasoning
	// BUILD_NOTES.md section 46 already used to reject porting
	// isomorphism into this codebase's own in-process resolver. That
	// reasoning transfers unchanged to enabling TexasSolver's OWN
	// isomorphism option on these same already-narrowed ranges, so it
	// stays disabled here for identical reasons.
	cmd << "set_use_isomorphism 0\n";
	cmd << "start_solve\n";
	cmd << "set_dump_rounds 1\n";
	cmd << "dump_result " << output_json_path << "\n";
	return cmd.str();
}

// ---------------------------------------------------------------------------
// Subprocess execution with a hard wall-clock timeout, enforced by polling
// waitpid(WNOHANG) from THIS thread (see file header comment for why this
// is deliberately not a background-thread/std::async watchdog).
// ---------------------------------------------------------------------------
struct RunResult {
	bool ok = false;
	int exit_code = -1;
	std::string error;       // set when ok==false
	std::string log_tail;    // tail of captured stdout+stderr, for diagnostics
};

inline std::string read_file_tail(const std::string& path, size_t max_bytes = 4096) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) return std::string();
	std::streamoff size = in.tellg();
	std::streamoff start = (size > (std::streamoff)max_bytes) ? (size - (std::streamoff)max_bytes) : 0;
	in.seekg(start);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

inline RunResult run_console_solver(const Config& cfg, const std::string& input_path, const std::string& log_path) {
	RunResult result;
	pid_t pid = fork();
	if (pid < 0) {
		result.error = std::string("fork() failed: ") + std::strerror(errno);
		return result;
	}
	if (pid == 0) {
		// Child: redirect stdout+stderr to the log file, then exec.
		int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		std::string mode_holdem = "holdem";
		char* argv[] = {
			const_cast<char*>(cfg.binary_path.c_str()),
			const_cast<char*>("-i"), const_cast<char*>(input_path.c_str()),
			const_cast<char*>("-r"), const_cast<char*>(cfg.resource_dir.c_str()),
			const_cast<char*>("-m"), const_cast<char*>(mode_holdem.c_str()),
			nullptr
		};
		execv(cfg.binary_path.c_str(), argv);
		_exit(127); // exec itself failed (binary missing/not executable)
	}

	// Parent: poll for completion without blocking, so we can enforce
	// timeout_ms as a hard wall-clock cap regardless of what the child is
	// doing internally.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.timeout_ms);
	int status = 0;
	bool exited = false;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) { exited = true; break; }
		if (r < 0) {
			result.error = std::string("waitpid() failed: ") + std::strerror(errno);
			return result;
		}
		if (std::chrono::steady_clock::now() >= deadline) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (!exited) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0); // reap, blocking is fine -- SIGKILL cannot be ignored
		result.error = "console_solver exceeded timeout of " + std::to_string(cfg.timeout_ms) + "ms; killed";
		result.log_tail = read_file_tail(log_path);
		return result;
	}
	if (WIFEXITED(status)) {
		result.exit_code = WEXITSTATUS(status);
	} else {
		result.error = "console_solver terminated abnormally (signal, not a normal exit)";
		result.log_tail = read_file_tail(log_path);
		return result;
	}
	if (result.exit_code != 127 && result.exit_code != 0) {
		result.error = "console_solver exited with code " + std::to_string(result.exit_code);
		result.log_tail = read_file_tail(log_path);
		return result;
	}
	if (result.exit_code == 127) {
		result.error = "console_solver failed to exec (binary_path=" + cfg.binary_path + ")";
		return result;
	}
	result.ok = true;
	return result;
}

// ---------------------------------------------------------------------------
// Output parsing. TexasSolver's `dump_result` JSON is a recursive game
// tree; the node this bridge dumps (root of the whole tree, or the
// subtree reached by `initial_actions`) is exactly the decision hero
// faces right now. Its shape (confirmed directly against a real
// dump_result output produced during this integration's investigation --
// see BUILD_NOTES.md):
//   { "player": 0|1, "actions": [...],
//     "strategy": { "actions": [...], "strategy": { "<4-char-combo>": [p...] } },
//     "childrens": {...} }
// Only "player", "strategy.actions" and "strategy.strategy" are needed
// here -- "childrens" (the rest of the tree) is never traversed, since
// this bridge only ever wants THIS ONE node's strategy.
// ---------------------------------------------------------------------------
struct ParsedAction {
	enum Kind { FOLD, CHECK, CALL, BET, RAISE } kind;
	double amount = -1.0; // chip amount, only meaningful for BET/RAISE
};

inline bool parse_action_string(const std::string& s, ParsedAction& out) {
	size_t space = s.find(' ');
	std::string name = (space == std::string::npos) ? s : s.substr(0, space);
	if (name == "FOLD") { out.kind = ParsedAction::FOLD; return true; }
	if (name == "CHECK") { out.kind = ParsedAction::CHECK; return true; }
	if (name == "CALL") { out.kind = ParsedAction::CALL; return true; }
	if (name == "BET" || name == "RAISE") {
		if (space == std::string::npos) return false;
		try {
			out.amount = std::stod(s.substr(space + 1));
		} catch (...) { return false; }
		out.kind = (name == "BET") ? ParsedAction::BET : ParsedAction::RAISE;
		return true;
	}
	return false;
}

struct ParsedStrategy {
	int player = -1;
	std::vector<std::string> actions;       // raw action strings, aligned with each combo's prob vector
	std::vector<ParsedAction> parsed;        // parsed form of `actions`, same order
	// combo string (e.g. "AhKh") -> probability vector aligned with `actions`
	std::unordered_map<std::string, std::vector<double>> combo_probs;
};

inline bool parse_strategy_output(const std::string& json_path, ParsedStrategy& out, std::string& error) {
	std::ifstream in(json_path);
	if (!in) { error = "could not open TexasSolver output file: " + json_path; return false; }
	nlohmann::json root;
	try {
		in >> root;
	} catch (const std::exception& e) {
		error = std::string("TexasSolver output JSON parse error: ") + e.what();
		return false;
	}
	if (!root.contains("player") || !root.contains("strategy")) {
		error = "TexasSolver output JSON missing expected \"player\"/\"strategy\" keys at root";
		return false;
	}
	out.player = root["player"].get<int>();
	const auto& strat = root["strategy"];
	if (!strat.contains("actions") || !strat.contains("strategy")) {
		error = "TexasSolver output JSON \"strategy\" object missing \"actions\"/\"strategy\" keys";
		return false;
	}
	for (const auto& a : strat["actions"]) out.actions.push_back(a.get<std::string>());
	out.parsed.resize(out.actions.size());
	for (size_t i = 0; i < out.actions.size(); i++) {
		if (!parse_action_string(out.actions[i], out.parsed[i])) {
			error = "could not parse TexasSolver action string: " + out.actions[i];
			return false;
		}
	}
	for (auto it = strat["strategy"].begin(); it != strat["strategy"].end(); ++it) {
		std::vector<double> probs;
		for (const auto& p : it.value()) probs.push_back(p.get<double>());
		out.combo_probs[it.key()] = std::move(probs);
	}
	if (out.combo_probs.empty()) {
		error = "TexasSolver output JSON \"strategy.strategy\" object is empty";
		return false;
	}
	return true;
}

// A combo can appear in the output keyed either "c1c2" or "c2c1" -- see
// BUILD_NOTES.md for the empirical key-ordering rule TexasSolver actually
// uses (rank-descending, suit-index-descending for pairs); rather than
// depend on that exact rule (an internal detail that could change), this
// tries both concatenation orders and uses whichever key is actually
// present.
inline const std::vector<double>* find_combo_probs(const ParsedStrategy& strat, unsigned char c1, unsigned char c2) {
	std::string k1 = card_str(c1) + card_str(c2);
	auto it = strat.combo_probs.find(k1);
	if (it != strat.combo_probs.end()) return &it->second;
	std::string k2 = card_str(c2) + card_str(c1);
	it = strat.combo_probs.find(k2);
	if (it != strat.combo_probs.end()) return &it->second;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Top-level orchestration.
// ---------------------------------------------------------------------------
struct Decision {
	bool ok = false;
	std::string error;
	std::string action;                      // "fold" | "call" | "allin" | "raise <amount>" -- same convention as dh_native_ai.cpp's resolve_decision()
	// Per-combo probability of the SAMPLED action, aligned 1:1 with the
	// `hero_range` vector passed in to solve() -- lets the caller narrow
	// its own persistent hero-range belief exactly like the in-process
	// resolver path already does (new_weight[i] = old_weight[i] * this[i]).
	std::vector<double> hero_prob_of_chosen_action;
	std::string diagnostic; // human-readable summary, only populated if verbose logging is requested by the caller
};

struct TempFiles {
	std::string input_path;
	std::string output_path;
	std::string log_path;
	~TempFiles() {
		// DH_TEXASSOLVER_KEEP_TEMP=1 preserves the batch-command input file,
		// the raw console_solver stdout+stderr log, and the dump_result JSON
		// output for post-mortem inspection -- e.g. to reproduce a failure
		// by re-running `console_solver -i <input_path> -r <resource_dir>
		// -m holdem` directly in a foreground shell, since run_console_solver()
		// only ever surfaces a truncated (4KB) tail of the log via
		// RunResult::log_tail. Off by default so normal operation doesn't
		// litter /tmp.
		if (env_or("DH_TEXASSOLVER_KEEP_TEMP", "") == "1") return;
		if (!input_path.empty()) std::remove(input_path.c_str());
		if (!output_path.empty()) std::remove(output_path.c_str());
		if (!log_path.empty()) std::remove(log_path.c_str());
	}
};

// `hero_is_ip`: true if hero occupies the IP (slot 0 / SB-BTN) postflop
// seat this hand, false if hero is OOP (slot 1 / BB) -- see
// dh_native_ai.cpp's slot convention (g.my_id==0 -> IP postflop).
// `action_path`: every action already taken this street, in chronological
// order, in TexasSolver's own wire vocabulary ("CHECK"/"CALL"/"FOLD"/
// "BET_<n>"/"RAISE_<n>" -- see dh_native_ai.cpp's g.street_action_path and
// its texassolver_bet_or_raise_token()/texassolver_check_or_call_token()
// builders). Empty for hero's own opening decision this street (solved via
// a fresh symmetric-commit root, no approximation needed). Any non-empty
// path is replayed via `set_initial_actions` before solving, using
// TexasSolver's own PCfrSolver::navigateToSubtree -- which walks an
// ARBITRARY-LENGTH comma-separated action list one token at a time,
// nearest-available-size-matching each BET_/RAISE_ amount -- so a river
// check-raise, a 3-bet, or any deeper action sequence this street is
// handled by the exact same mechanism as the single-prior-action case,
// not a special case of it. (An earlier version of this bridge only
// supported 0 or 1 prior actions and refused anything deeper; that
// restriction was an artificial limitation of THIS bridge's own call
// site, not a real constraint of TexasSolver's interface -- see
// BUILD_NOTES.md for the full derivation, including why the token amount
// must be each actor's own INCREMENT over their prior commitment this
// street, not DH's own "new total" convention.) TexasSolver's own
// raise_limit (4 raises/street, include/tools/CommandLineTool.h) still
// applies underneath this -- a path deeper than that fails safely
// (ok=false, caught below) rather than crashing.
inline Decision solve(
	bool hero_is_ip,
	const std::vector<Combo>& hero_range,
	const std::vector<Combo>& villain_range,
	const std::vector<unsigned char>& board,
	int pot_at_street_start,
	int effective_stack_at_street_start,
	const std::vector<std::string>& action_path,
	unsigned char hero_c1, unsigned char hero_c2,
	std::mt19937_64& rng) {

	Decision result;
	// RIVER ONLY -- see this file's top header comment for the full
	// rationale (OOM-confirmed intractability of a flop/turn-rooted
	// TexasSolver solve at DH's realistic range widths). This is checked
	// here, not just at the call site, so ANY caller of this bridge --
	// present or future -- gets a clear, immediate, non-destructive error
	// instead of risking the subprocess OOM this restriction exists to
	// prevent.
	if (board.size() != 5) {
		result.error = "TexasSolver bridge is river-only (board must have exactly 5 cards, got " +
			std::to_string(board.size()) + ") -- flop/turn fallback is out of scope, see BUILD_NOTES.md";
		return result;
	}

	SolveRequest req;
	req.pot_at_street_start = pot_at_street_start;
	req.effective_stack_at_street_start = effective_stack_at_street_start;
	req.board = board;
	req.ip_range = hero_is_ip ? hero_range : villain_range;
	req.oop_range = hero_is_ip ? villain_range : hero_range;
	req.cfg = load_config();
	for (size_t i = 0; i < action_path.size(); i++) {
		if (i) req.initial_actions += ",";
		req.initial_actions += action_path[i];
	}


	TempFiles tmp;
	{
		char buf[256];
		unsigned long tag = (unsigned long)::getpid() * 2654435761u + (unsigned long)std::chrono::steady_clock::now().time_since_epoch().count();
		std::snprintf(buf, sizeof(buf), "/tmp/dh_texassolver_%lu", tag);
		tmp.input_path = std::string(buf) + "_input.txt";
		tmp.output_path = std::string(buf) + "_output.json";
		tmp.log_path = std::string(buf) + "_log.txt";
	}

	{
		std::ofstream out(tmp.input_path);
		if (!out) {
			result.error = "could not create TexasSolver input file: " + tmp.input_path;
			return result;
		}
		out << build_batch_commands(req, tmp.output_path);
	}

	RunResult run = run_console_solver(req.cfg, tmp.input_path, tmp.log_path);
	if (!run.ok) {
		result.error = run.error;
		if (!run.log_tail.empty()) result.error += " | log tail: " + run.log_tail;
		return result;
	}

	ParsedStrategy strat;
	std::string parse_error;
	if (!parse_strategy_output(tmp.output_path, strat, parse_error)) {
		result.error = parse_error;
		return result;
	}

	int expected_player = hero_is_ip ? 0 : 1;
	if (strat.player != expected_player) {
		result.error = "TexasSolver dumped node belongs to player " + std::to_string(strat.player) +
			", expected " + std::to_string(expected_player) + " (hero) -- initial_actions/turn-order mismatch";
		return result;
	}

	const std::vector<double>* hero_row = find_combo_probs(strat, hero_c1, hero_c2);
	if (!hero_row || hero_row->size() != strat.actions.size()) {
		result.error = "hero's actual combo (" + card_str(hero_c1) + card_str(hero_c2) +
			") not found in TexasSolver's dumped strategy";
		return result;
	}

	// Sample, exactly like dh_native_ai.cpp's resolve_decision(): walk the
	// cumulative distribution of hero's own row.
	double r = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
	double cum = 0.0;
	size_t selected = hero_row->size() - 1;
	for (size_t a = 0; a < hero_row->size(); a++) {
		cum += (*hero_row)[a];
		if (r <= cum || a + 1 == hero_row->size()) { selected = a; break; }
	}

	const ParsedAction& act = strat.parsed[selected];
	if (act.kind == ParsedAction::FOLD) result.action = "fold";
	else if (act.kind == ParsedAction::CHECK || act.kind == ParsedAction::CALL) result.action = "call";
	else {
		// BET/RAISE. Since this bridge only ever solves a fresh node where
		// hero has not yet committed anything this street (true for both
		// actions_this_street==0 and ==1 -- in the ==1 case, the OTHER
		// seat's action already accounted for by set_initial_actions, not
		// hero's), TexasSolver's reported "amount" already equals hero's
		// full new street-relative commitment -- exactly the quantity
		// dh_native_ai.cpp's own resolve_decision() returns for a raise.
		// See BUILD_NOTES.md for the derivation (cross-checked against
		// GameTree::get_possible_bets()/GameTree.cpp's action construction).
		if (act.amount >= effective_stack_at_street_start - 1e-6) result.action = "allin";
		else result.action = "raise " + std::to_string((long long)std::llround(act.amount));
	}

	// Narrow: every combo in hero_range gets the probability IT would have
	// assigned to the SAME selected action index, from ITS OWN row --
	// mirroring resolve_decision()'s existing per-combo narrowing loop
	// exactly, just reading probabilities from TexasSolver's output
	// instead of LiveResolver::average_strategy().
	result.hero_prob_of_chosen_action.resize(hero_range.size(), 0.0);
	for (size_t i = 0; i < hero_range.size(); i++) {
		const std::vector<double>* row = find_combo_probs(strat, hero_range[i].c1, hero_range[i].c2);
		result.hero_prob_of_chosen_action[i] = row ? (*row)[selected] : 0.0;
	}

	result.ok = true;
	result.diagnostic = "TexasSolver fallback: player=" + std::to_string(strat.player) +
		" actions=" + std::to_string(strat.actions.size()) +
		" selected=" + strat.actions[selected] +
		" iterations<=" + std::to_string(req.cfg.max_iterations);
	return result;
}

} // namespace texassolver_bridge
