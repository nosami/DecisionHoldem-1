//###############################################################################
//   time_per_file_river_load.cpp -- REAL (not extrapolated) timing for
//   loading a subset of hole-hands' worth of river-cluster data via the
//   per-file split (split_cluster_file.cpp output), measured directly
//   against the actual river_hand_cluster.bin data (12.71MB/hand blocks).
//   No Engine/RAM-loading involved -- this host cannot fully load
//   river_cluster[] (needs ~16.86GB, exceeds 16GB total RAM, see
//   BUILD_NOTES.md section 29), so this measures raw per-file I/O only.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -o tools/time_per_file_river_load tools/time_per_file_river_load.cpp
//   RUN (from PokerAI/):
//     ./tools/time_per_file_river_load <split_dir> <community_total> <key_bytes> <val_bytes>
//###############################################################################
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
	long long block_size = key_bytes * community_total + val_bytes * community_total;

	std::mt19937 rng(123);
	std::uniform_int_distribution<int> card_dist(0, 51);

	std::printf("Block size: %lld bytes (%.2f MiB) per hole-hand\n\n", block_size, block_size / (1024.0*1024.0));
	std::printf("=== Timing: loading a SUBSET of hole-hands, one file per hand (REAL river data, cold-ish) ===\n");
	for (int n : { 50, 100, 200, 500, 1000, 1326 }) {
		std::set<int> handids;
		while ((int)handids.size() < n) {
			int i, j;
			do { i = card_dist(rng); j = card_dist(rng); } while (i >= j);
			handids.insert(i * 52 + j);
		}
		std::vector<char> buf(block_size);
		auto s0 = std::chrono::steady_clock::now();
		long long total_bytes = 0;
		for (int handid : handids) {
			std::string path = dir + "/" + std::to_string(handid) + ".bin";
			std::ifstream f(path, std::ios::binary);
			f.read(buf.data(), (long long)buf.size());
			total_bytes += buf.size();
		}
		auto s1 = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(s1 - s0).count();
		double gib = total_bytes / (1024.0 * 1024.0 * 1024.0);
		std::printf("  %5d files: %9.1fms wall  (%.2f GiB, %.1f MB/s effective)\n",
			n, ms, gib, (total_bytes / (1024.0*1024.0)) / (ms / 1000.0));
	}
	return 0;
}
