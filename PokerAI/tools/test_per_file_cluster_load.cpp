//###############################################################################
//   test_per_file_cluster_load.cpp -- validates the user-proposed
//   "one file per hole-hand key" alternative to offset-seeking into one
//   giant file (see BUILD_NOTES.md section 30 for the offset-based version;
//   this is the filename-based version, produced by split_cluster_file.cpp).
//
//   Loads ONLY the specific per-hand files needed (by filename, no offset
//   math at all -- "<out_dir>/<handid>.bin" where handid = i*52+j), and
//   cross-checks the same binary search find_turn()/find_river() does
//   against Engine's fully-loaded in-RAM ground truth, plus times loading
//   subsets of various sizes.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -o tools/test_per_file_cluster_load tools/test_per_file_cluster_load.cpp
//   RUN (from PokerAI/), pointing at a directory already produced by
//   split_cluster_file.cpp:
//     ./tools/test_per_file_cluster_load <split_dir> <community_total> <key_bytes> <val_bytes>
//###############################################################################
#include "../poker/Engine.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <random>
#include <fstream>
#include <set>
#include <string>

int main(int argc, char** argv) {
	if (argc != 5) {
		std::fprintf(stderr, "usage: %s <split_dir> <community_total> <key_bytes> <val_bytes>\n", argv[0]);
		return 1;
	}
	std::string dir = argv[1];
	long long community_total = atoll(argv[2]);
	long long key_bytes = atoll(argv[3]);
	long long val_bytes = atoll(argv[4]);

	// Ground truth: full Engine load (river skipped, unaffected by this test).
	std::printf("=== Loading full Engine (ground truth) ===\n");
	auto t0 = std::chrono::steady_clock::now();
	Engine* eng = new Engine();
	auto t1 = std::chrono::steady_clock::now();
	std::printf("Full Engine::load(): %.1fms\n",
		std::chrono::duration<double, std::milli>(t1 - t0).count());

	std::mt19937 rng(7);
	std::uniform_int_distribution<int> card_dist(0, 51);

	std::printf("\n=== Correctness: per-file load vs full in-RAM Engine ===\n");
	int n_checks = 30, passed = 0;
	for (int t = 0; t < n_checks; t++) {
		int i, j;
		do { i = card_dist(rng); j = card_dist(rng); } while (i >= j);
		int handid = i * 52 + j;
		std::string path = dir + "/" + std::to_string(handid) + ".bin";

		std::vector<unsigned> keys(community_total), values(community_total);
		std::ifstream f(path, std::ios::binary);
		if (!f) { std::printf("  hand(%2d,%2d) FILE MISSING: %s\n", i, j, path.c_str()); continue; }
		f.read((char*)keys.data(), key_bytes * community_total);
		f.read((char*)values.data(), val_bytes * community_total);

		unsigned communityid = keys[community_total / 3];
		int left = 0, right = (int)community_total - 1, found_idx = -1;
		while (left <= right) {
			int mid = (left + right) / 2;
			if (keys[mid] == communityid) { found_idx = mid; break; }
			if (communityid > keys[mid]) left = mid + 1; else right = mid - 1;
		}
		unsigned per_file_result = (found_idx >= 0) ? values[found_idx] : 0xFFFFFFFF;
		unsigned full_result = eng->find_turn((unsigned)handid, communityid);

		bool ok = (found_idx >= 0) && (per_file_result == full_result);
		if (ok) passed++;
		if (t < 5 || !ok)
			std::printf("  hand(%2d,%2d) key=%10u  per_file=%6u  full=%6u  %s\n",
				i, j, communityid, per_file_result, full_result, ok ? "OK" : "MISMATCH");
	}
	std::printf("Result: %d/%d per-file lookups matched full in-RAM Engine exactly\n", passed, n_checks);

	std::printf("\n=== Timing: loading a SUBSET of hole-hands, one file per hand ===\n");
	for (int n : { 50, 200, 500, 1000, 1326 }) {
		std::set<int> handids;
		while ((int)handids.size() < n) {
			int i, j;
			do { i = card_dist(rng); j = card_dist(rng); } while (i >= j);
			handids.insert(i * 52 + j);
		}
		auto s0 = std::chrono::steady_clock::now();
		long long total_bytes = 0;
		for (int handid : handids) {
			std::string path = dir + "/" + std::to_string(handid) + ".bin";
			std::ifstream f(path, std::ios::binary);
			std::vector<char> buf(key_bytes * community_total + val_bytes * community_total);
			f.read(buf.data(), (long long)buf.size());
			total_bytes += buf.size();
		}
		auto s1 = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(s1 - s0).count();
		std::printf("  %5d files: %8.1fms wall  (%.1f MiB, %d individual open+read+close calls)\n",
			n, ms, total_bytes / (1024.0 * 1024.0), n);
	}
	return 0;
}
