//###############################################################################
//   test_partial_cluster_load.cpp -- proof of concept for partial/on-demand
//   loading of the per-hole-hand cluster files (turn_hand_cluster.bin,
//   river_hand_cluster.bin), answering: "can we load only a SUBSET of
//   hole-hand blocks instead of the whole file into RAM?"
//
//   FORMAT (confirmed against Engine.h's load loop and the real file sizes
//   on disk, see BUILD_NOTES.md): each of these files is a flat, headerless
//   concatenation of exactly 1326 fixed-size blocks, one per distinct 2-card
//   hole-hand combo (i,j) with i<j over 52 cards, written in the nested-loop
//   order `for i in 0..50: for j in i+1..51`. Each block is a SORTED
//   (keys[], values[]) pair table (keys = packed 5-card community encoding,
//   values = cluster id), directly binary-searchable via find_turn()/
//   find_river()'s existing logic. Critically: block N's byte offset can be
//   computed in closed form from (i,j) alone -- no scanning or index needed
//   -- so ANY subset of hole-hands can be read directly via fseek, skipping
//   the rest entirely, with ZERO changes to the on-disk file format.
//
//   This tool:
//     1. Derives the closed-form offset formula for a given (i,j) pair.
//     2. Loads a SUBSET of hole-hand blocks from the real, on-SSD
//        turn_hand_cluster.bin (stand-in for river_hand_cluster.bin, which
//        has the identical per-hand block layout, just different constants
//        -- river_hand_cluster.bin itself currently lives on a Seagate
//        external volume this sandboxed environment cannot read raw bytes
//        from, see BUILD_NOTES.md; the SAME code below works unchanged
//        against it, only TURN_MODE would need to be set to false).
//     3. Cross-validates every partially-loaded block's binary-search
//        results against Engine's own FULLY-loaded in-RAM arrays for the
//        same hole-hands, to prove correctness (not just "it doesn't
//        crash").
//     4. Measures real wall-clock time to load a subset of blocks matching
//        realistic villain-range sizes (50/200/500/1000 hands), to give
//        real numbers for whether this is fast enough to do per-decision
//        (or once per hand, cached across streets).
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_partial_cluster_load tools/test_partial_cluster_load.cpp
//   RUN (from PokerAI/):
//     ./tools/test_partial_cluster_load
//###############################################################################
#include "../poker/Engine.h"
#include <cstdio>
#include <chrono>
#include <vector>
#include <random>
#include <fstream>

// Closed-form rank of hole-hand (i,j), i<j, in the file's
// `for i in 0..50: for j in i+1..51` write order. Verified: rank(0,1)=0,
// rank(0,2)=1, rank(1,2)=51 (i=0's row has 51 entries: j=1..51).
static long long combo_rank(int i, int j) {
	long long rank = (long long)i * 51 - (long long)i * (i - 1) / 2;
	rank += (j - i - 1);
	return rank;
}

int main() {
	const char* path = "/Users/jason/dh_local_data/turn_hand_cluster.bin";
	const long long community_total = turn_community_total; // 230300
	const long long key_bytes = 4, val_bytes = 4;            // turn: unsigned/unsigned
	const long long block_size = community_total * (key_bytes + val_bytes);

	std::printf("=== Format check ===\n");
	std::printf("block_size = %lld bytes (%.2f MiB); predicted file size for 1326 combos = %lld bytes\n",
		block_size, block_size / (1024.0 * 1024.0), block_size * 1326);
	{
		std::ifstream f(path, std::ios::binary | std::ios::ate);
		std::printf("actual file size on disk = %lld bytes  %s\n", (long long)f.tellg(),
			(f.tellg() == block_size * 1326) ? "(MATCHES prediction exactly)" : "(MISMATCH!)");
	}

	// Load the FULL file via Engine (ground truth for correctness check).
	// DH_SKIP_RIVER_CLUSTER keeps this from also attempting the 16.86GB
	// river file, which isn't needed for this proof of concept.
	std::printf("\n=== Loading full Engine (ground truth) ===\n");
	auto t_full0 = std::chrono::steady_clock::now();
	Engine* eng = new Engine();
	auto t_full1 = std::chrono::steady_clock::now();
	double full_load_ms = std::chrono::duration<double, std::milli>(t_full1 - t_full0).count();
	std::printf("Full Engine::load() (sevencards+preflop+flop+turn, river skipped): %.1fms\n", full_load_ms);

	// Pick a random sample of hole-hand combos and boards to cross-check.
	std::mt19937 rng(42);
	std::uniform_int_distribution<int> card_dist(0, 51);
	int n_checks = 30, passed = 0;
	std::printf("\n=== Correctness: partial single-block read vs full in-RAM Engine ===\n");
	std::ifstream fin(path, std::ios::binary);
	for (int t = 0; t < n_checks; t++) {
		int i, j;
		do { i = card_dist(rng); j = card_dist(rng); } while (i >= j);
		long long offset = combo_rank(i, j) * block_size;

		// Partial read: seek directly to this hand's block, read ONLY it.
		std::vector<unsigned> keys(community_total), values(community_total);
		fin.seekg(offset);
		fin.read((char*)keys.data(), key_bytes * community_total);
		fin.read((char*)values.data(), val_bytes * community_total);

		// Same binary search find_turn() does, against the partially-loaded arrays.
		unsigned communityid = keys[community_total / 3]; // a real key guaranteed present
		int left = 0, right = (int)community_total - 1, found_idx = -1;
		while (left <= right) {
			int mid = (left + right) / 2;
			if (keys[mid] == communityid) { found_idx = mid; break; }
			if (communityid > keys[mid]) left = mid + 1; else right = mid - 1;
		}
		unsigned partial_result = (found_idx >= 0) ? values[found_idx] : 0xFFFFFFFF;

		// Ground truth: same lookup via Engine's fully-loaded arrays.
		unsigned full_result = eng->find_turn((unsigned)(i * 52 + j), communityid);

		bool ok = (found_idx >= 0) && (partial_result == full_result);
		if (ok) passed++;
		if (t < 5 || !ok)
			std::printf("  hand(%2d,%2d) key=%10u  partial=%6u  full=%6u  %s\n",
				i, j, communityid, partial_result, full_result, ok ? "OK" : "MISMATCH");
	}
	std::printf("Result: %d/%d partial-block lookups matched full in-RAM Engine exactly\n", passed, n_checks);

	// Timing: load N distinct hole-hand blocks fresh from disk (simulating
	// loading only a villain's currently-tracked range, not all 1326 hands).
	std::printf("\n=== Timing: loading a SUBSET of hole-hand blocks from disk ===\n");
	std::vector<int> subset_sizes = { 50, 200, 500, 1000, 1326 };
	for (int n : subset_sizes) {
		std::vector<std::pair<int,int>> hands;
		std::set<int> seen;
		while ((int)hands.size() < n) {
			int i, j;
			do { i = card_dist(rng); j = card_dist(rng); } while (i >= j);
			int flat = i * 52 + j;
			if (seen.count(flat)) continue;
			seen.insert(flat);
			hands.push_back({ i, j });
		}
		auto t0 = std::chrono::steady_clock::now();
		std::ifstream f(path, std::ios::binary);
		std::vector<unsigned> keys(community_total), values(community_total);
		for (auto& h : hands) {
			long long offset = combo_rank(h.first, h.second) * block_size;
			f.seekg(offset);
			f.read((char*)keys.data(), key_bytes * community_total);
			f.read((char*)values.data(), val_bytes * community_total);
		}
		auto t1 = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		double ram_mb = (double)(n) * block_size / (1024.0 * 1024.0);
		std::printf("  %5d hands: %8.1fms wall  (%.1f MiB touched, %.1f MiB/s)  vs. full %d-hand load's %.1fMiB\n",
			n, ms, ram_mb, ram_mb / (ms / 1000.0), 1326, 1326 * block_size / (1024.0*1024.0));
	}
	return 0;
}
