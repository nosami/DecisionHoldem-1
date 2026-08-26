//###############################################################################
//   BlueprintReader.h -- NEW, ORIGINAL, ADDITIVE code (not part of the
//   original DecisionHoldem sources, not a modification of Save_load.h).
//
//   PokerAI/tree/Save_load.h's dump()/load() functions are the ORIGINAL
//   authors' own (de)serializer for a trained CFR blueprint
//   (cluster/blueprint_strategy.dat, ~16.1GB). load() works by fully
//   materializing the ENTIRE recursive strategy_node tree in memory before
//   any single lookup can be performed -- there is no random access, and no
//   way to ask "just give me hero's strategy at this one decision point"
//   without first paying the cost (RAM + time) of loading everything.
//
//   This header implements a MUCH smaller, targeted, streaming reader that
//   walks the *exact same* on-disk format (reverse-engineered directly from
//   Save_load.h's dfs_write()/bulid_bluestrategy(), which this file mirrors
//   byte-for-byte) but only ever reads the handful of node headers on the
//   path to ONE specific decision point, and never allocates or reads the
//   rest of the ~16GB file. This makes it possible to use the REAL, trained
//   preflop blueprint strategy for live decisions without loading the whole
//   tree into RAM (previously measured, in BUILD_NOTES.md, to risk >19GB of
//   swap growth for the full Engine + full blueprint combination).
//
//   ON-DISK FORMAT (see Save_load.h for the authoritative write-side code):
//     Each node, for a "batch" of `clusterlen` parallel private info-set
//     slots (169 preflop hand-cluster buckets at the root), is written as:
//       int32   action_len              (0 => terminal, no more data for
//                                         this node; shared by ALL
//                                         clusterlen slots)
//       uint8[action_len] actionstr     (only if action_len > 0; the legal
//                                         action byte codes at this node,
//                                         shared by all clusterlen slots --
//                                         see PokerAI/poker/State.h's
//                                         legal_actions()/take_action() for
//                                         what the byte codes mean: 'd'
//                                         fold, 'l' call/check, 'n' allin,
//                                         or a small integer pot-fraction
//                                         raise-size code such as 1/2/4/8/
//                                         20/40)
//       for each of the clusterlen slots, in order:
//         double[action_len] regret     (raw per-iteration regret -- not
//                                         needed for a lookup, only kept
//                                         reachable so the stream stays
//                                         aligned)
//         double[action_len] averegret  (the CFR *average strategy*
//                                         accumulator -- normalizing the
//                                         positive entries, exactly like
//                                         Node.h's own calculate_strategy(),
//                                         gives the actual trained strategy
//                                         to play)
//       then, recursively, action_len subtrees (one per legal action, same
//       clusterlen-wide format), each written in full before the next one
//       begins (depth-first, action-major order).
//
//     CORRECTION (found via a real live-play bug, see BUILD_NOTES.md section
//     21): an earlier version of this file assumed "preflop betting never
//     produces `action_len >= 100`" -- that is WRONG. Save_load.h's dump()
//     serializes the *entire* game tree in one file, not a preflop-only
//     slice. `action_len >= 100` is a chance-node marker (e.g. the flop
//     board deal), and while the exact node this reader is asked to
//     evaluate is indeed always still-preflop-and-open (guaranteed by the
//     caller only ever invoking a preflop lookup before betting closes), a
//     SIBLING action being skipped past (e.g. a check-back that closes
//     preflop betting) can legitimately lead straight into a chance node a
//     few levels inside its own subtree -- normal tree shape, not
//     corruption. `skip_subtree()` below now mirrors dfs_write()'s chance-
//     node branch (`else if (len > 100)`) exactly: a chance-node marker
//     carries no data of its own, and is immediately followed by exactly
//     one more node, read with clusterlen replaced by the chance node's
//     fan-out count. `read_node_header()` (used only for nodes actually on
//     the caller's lookup path) still throws if it ever sees `action_len >=
//     100`, since that would mean the caller asked for a "preflop" decision
//     at a node where preflop has already closed -- a real caller bug worth
//     failing loudly on, not something to silently paper over.
//
//   PERFORMANCE NOTE: because a skipped sibling can now legitimately
//   contain an entire postflop subgame (everything hanging off a closed
//   preflop line), `skip_subtree()` is no longer guaranteed to cost only a
//   few KB for every lookup -- a path that skips past an early-closing
//   sibling (e.g. skipping over a "check back the limp" branch to reach a
//   raise branch) will sequentially read through that sibling's full
//   nested tree on disk (still far less than the whole ~16GB file, and
//   still no large in-RAM allocation, but not always negligible I/O).
//
//   HONEST VALIDATION STATUS (see BUILD_NOTES.md for the full writeup):
//   this reader was written by careful, byte-for-byte inspection of
//   Save_load.h's write-side code. Its *root-level* read (empty
//   action_path) was validated by the user against the real
//   cluster/blueprint_strategy.dat (PokerAI/tools/test_blueprint_root_read.cpp,
//   see BUILD_NOTES.md section 18) and produced sane, non-degenerate,
//   correctly-normalized real strategy data. The chance-node-skip fix above
//   was written in response to a real exception the user hit during actual
//   live play (BUILD_NOTES.md section 21) and reasoned through directly
//   against Save_load.h's write-side code, but -- like all non-root paths
//   through this reader -- has not yet been re-validated against the real
//   file from within this development sandbox, which still lacks OS-level
//   permission to read files on the external drive that hosts it. Re-test
//   live play to confirm the fix; if any further "unexpected chance node"
//   or "action byte not found" errors surface, they indicate either another
//   reader bug or a `preflop_action_path` tracking bug in dh_native_ai.cpp
//   (see resolve_preflop_decision()'s try/catch there for how these are
//   reported without crashing the whole hand).
//###############################################################################
#pragma once
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>

namespace BlueprintReader {

// One node's header, read directly off disk -- see the format description
// above. Only `averegret` is retained (that's what a lookup needs); `regret`
// is read-and-discarded just to keep the stream positioned correctly.
struct NodeHeader {
	int action_len = 0; // 0 => terminal node, no actionstr/regret data follows
	std::vector<unsigned char> actionstr;       // [action_len]
	std::vector<std::vector<double>> averegret; // [clusterlen][action_len]
};

// Reads exactly one node header from `fin` (already positioned at the start
// of that node's data), for a node spanning `clusterlen` parallel private
// info-set slots. Throws std::runtime_error on EOF, on an unsupported
// chance-node marker (action_len >= 100), or any other format surprise --
// never silently misinterprets raw bytes as strategy data.
inline NodeHeader read_node_header(std::ifstream& fin, int clusterlen) {
	NodeHeader h;
	fin.read(reinterpret_cast<char*>(&h.action_len), sizeof(int));
	if (!fin) throw std::runtime_error("BlueprintReader: unexpected EOF reading a node's action_len");
	if (h.action_len <= 0) { h.action_len = 0; return h; }
	if (h.action_len >= 100)
		throw std::runtime_error(
			"BlueprintReader: encountered a chance-node marker (action_len >= 100) "
			"while navigating what should be a preflop-only path -- refusing to "
			"trust nearby bytes as strategy data (this means our navigation of the "
			"tree took a wrong turn, most likely an action byte that doesn't "
			"actually match this training run's tree)");

	h.actionstr.resize(h.action_len);
	fin.read(reinterpret_cast<char*>(h.actionstr.data()), h.action_len);
	if (!fin) throw std::runtime_error("BlueprintReader: unexpected EOF reading actionstr");

	std::vector<double> regret_scratch(h.action_len); // read-and-discard, keeps stream aligned
	h.averegret.assign(clusterlen, std::vector<double>(h.action_len));
	for (int i = 0; i < clusterlen; i++) {
		fin.read(reinterpret_cast<char*>(regret_scratch.data()), sizeof(double) * h.action_len);
		fin.read(reinterpret_cast<char*>(h.averegret[i].data()), sizeof(double) * h.action_len);
		if (!fin) throw std::runtime_error("BlueprintReader: unexpected EOF reading regret/averegret arrays");
	}
	return h;
}

// Consumes (skips, without allocating or storing anything) one full subtree
// rooted at a node spanning `clusterlen` slots, mirroring the exact
// recursive order Save_load.h's dfs_write() used to write it, so `fin` ends
// up positioned exactly at whatever comes immediately after in the file.
//
// IMPORTANT (found via a real live-play bug report, see BUILD_NOTES.md):
// Save_load.h's dump() serializes the *entire* game tree in one file, not
// just the preflop portion. A SIBLING action we need to skip past (e.g. a
// check-back that closes preflop betting) can legitimately lead straight
// into a chance node (the flop deal) a few levels inside that sibling's own
// subtree -- that is normal, correct tree shape, not corruption. Per
// dfs_write()'s `else if (len > 100)` branch, a chance-node marker writes
// ONLY the int32 `len` itself (no actionstr/regret/averegret follow for the
// chance node), then makes EXACTLY ONE further recursive dfs_write() call
// with clusterlen REPLACED by `len` (each of the `len` chance outcomes
// becomes the new parallel-slot dimension for whatever betting node comes
// next, exactly like the initial 169 preflop hand-cluster slots). This is a
// single nested call, not a loop of `len` calls -- mirror that exactly here.
inline void skip_subtree(std::ifstream& fin, int clusterlen) {
	int len;
	fin.read(reinterpret_cast<char*>(&len), sizeof(int));
	if (!fin) throw std::runtime_error("BlueprintReader: unexpected EOF while skipping a subtree");
	if (len <= 0) return; // terminal node -- nothing else was written for it
	if (len >= 100) {
		// Chance node: no data of its own besides the int32 already read;
		// exactly one more node (with the new clusterlen) follows.
		skip_subtree(fin, len);
		return;
	}
	fin.seekg(static_cast<std::streamoff>(len), std::ios::cur); // actionstr
	fin.seekg(static_cast<std::streamoff>(clusterlen) * len * 2 * sizeof(double), std::ios::cur); // regret+averegret, all clusters
	for (int i = 0; i < len; i++) skip_subtree(fin, clusterlen);
}

// The result of a successful lookup: the legal action bytes at the target
// node, and the normalized average strategy (same order) for one specific
// 169-way preflop hand cluster (from Engine::get_preflop_cluster()).
struct LookupResult {
	std::vector<unsigned char> actionstr;
	std::vector<double> probs;
};

// Walks from the blueprint tree's root down through `action_path` (each
// byte must exactly match one of PokerAI/poker/State.h's own action codes:
// 'd' fold, 'l' call/check, 'n' allin, or an integer raise-size code), then
// reads that final node's header and returns hero's normalized average
// strategy for `hand_cluster` (0-168).
//
// CORRECTION (see BUILD_NOTES.md section 23 -- this comment previously,
// incorrectly, claimed the cost here is always small/bounded): that is
// only true when every sibling action skipped along the way is itself
// terminal (fold) or a plain betting node. Per the note on skip_subtree()
// above, a sibling that CLOSES preflop betting (most commonly 'l' -- a
// call/limp, which is almost always the first real action in each node's
// action list per State.h's ordering) opens straight into the entire
// nested postflop (flop/turn/river) subtree for that betting line, which
// can be a large fraction of the ~16GB file. Reaching any action listed
// AFTER 'l' therefore requires fully walking past that whole subtree's
// node headers (still no large in-RAM allocation or bulk payload reads --
// skip_subtree() seeks over regret/averegret arrays rather than reading
// them -- but still one int32 read + one seek per node in that subtree).
// Measured on a local SSD copy of the real blueprint file, this costs
// roughly 6-10 seconds per such lookup; on the file's original location
// (an external HDD/USB drive), the same lookup was easily 100x+ slower
// due to per-seek latency, to the point of looking hung. See
// BUILD_NOTES.md section 23 for the measurements and the fix (copy the
// blueprint file to local fast storage; this header's logic did not
// change, only where the file lives).
inline LookupResult lookup_preflop_strategy(
	const std::string& blueprint_path,
	const std::vector<unsigned char>& action_path,
	int hand_cluster)
{
	std::ifstream fin(blueprint_path, std::ios::in | std::ios::binary);
	if (!fin) throw std::runtime_error("BlueprintReader: cannot open " + blueprint_path);

	const int CLUSTERS = 169;
	NodeHeader h;
	for (size_t step = 0; ; step++) {
		h = read_node_header(fin, CLUSTERS);
		if (h.action_len == 0)
			throw std::runtime_error(
				"BlueprintReader: reached a terminal (already-resolved) node before "
				"consuming the full action_path -- the path is inconsistent with the "
				"trained tree (e.g. a fold/allin should have ended the hand already)");
		if (step == action_path.size()) break; // this IS the target node
		int idx = -1;
		for (int i = 0; i < h.action_len; i++)
			if (h.actionstr[i] == action_path[step]) { idx = i; break; }
		if (idx < 0)
			throw std::runtime_error("BlueprintReader: action byte not found among this node's legal actions");
		for (int i = 0; i < idx; i++) skip_subtree(fin, CLUSTERS);
		// fin is now positioned at the start of action `idx`'s subtree --
		// the next loop iteration reads ITS header.
	}

	if (hand_cluster < 0 || hand_cluster >= CLUSTERS)
		throw std::runtime_error("BlueprintReader: hand_cluster out of range [0,169)");

	LookupResult r;
	r.actionstr = h.actionstr;
	r.probs.assign(h.action_len, 0.0);
	double sum = 0.0;
	for (int i = 0; i < h.action_len; i++)
		if (h.averegret[hand_cluster][i] > 0) sum += h.averegret[hand_cluster][i];
	if (!(sum > 0.0))
		throw std::runtime_error(
			"BlueprintReader: non-positive average-strategy sum for this hand cluster "
			"-- refusing to trust this read (Save_load.h's own loader asserts sum>0 "
			"for every cluster it reads, as a basic sanity check on real data)");
	for (int i = 0; i < h.action_len; i++)
		r.probs[i] = (h.averegret[hand_cluster][i] > 0) ? (h.averegret[hand_cluster][i] / sum) : 0.0;
	return r;
}

} // namespace BlueprintReader
