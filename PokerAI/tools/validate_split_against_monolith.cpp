//###############################################################################
//   validate_split_against_monolith.cpp -- byte-exact cross-check that
//   splitting a monolithic per-hole-hand cluster file into 1326 separate
//   per-hand files (split_cluster_file.cpp) preserved every byte correctly.
//
//   Does NOT go through Engine (river_cluster[] would need ~16.86GB RAM to
//   fully load, exceeding this host's 16GB total -- see BUILD_NOTES.md
//   section 29). Instead validates directly against the source monolithic
//   file itself: for a random sample of hole-hands, seeks to that hand's
//   block in the monolithic file (using the same combo_rank() offset
//   formula from test_partial_cluster_load.cpp) and compares every byte
//   to the corresponding standalone per-hand file.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -o tools/validate_split_against_monolith tools/validate_split_against_monolith.cpp
//   RUN (from PokerAI/):
//     ./tools/validate_split_against_monolith <monolith_file> <split_dir> <community_total> <key_bytes> <val_bytes> <n_samples>
//###############################################################################
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <random>

long long combo_rank(int i, int j) {
	long long rank = (long long)i * 51 - (long long)i * (i - 1) / 2;
	rank += (j - i - 1);
	return rank;
}

int main(int argc, char** argv) {
	if (argc != 7) {
		std::fprintf(stderr, "usage: %s <monolith_file> <split_dir> <community_total> <key_bytes> <val_bytes> <n_samples>\n", argv[0]);
		return 1;
	}
	std::string mono_path = argv[1];
	std::string split_dir = argv[2];
	long long community_total = atoll(argv[3]);
	long long key_bytes = atoll(argv[4]);
	long long val_bytes = atoll(argv[5]);
	int n_samples = atoi(argv[6]);
	long long block_size = community_total * (key_bytes + val_bytes);

	std::ifstream mono(mono_path, std::ios::binary);
	if (!mono) { std::fprintf(stderr, "cannot open monolith file %s\n", mono_path.c_str()); return 1; }

	std::mt19937 rng(42);
	std::uniform_int_distribution<int> card_dist(0, 51);

	std::vector<char> mono_buf(block_size), split_buf(block_size);
	int passed = 0;
	for (int t = 0; t < n_samples; t++) {
		int i, j;
		do { i = card_dist(rng); j = card_dist(rng); } while (i >= j);
		int handid = i * 52 + j;

		long long offset = combo_rank(i, j) * block_size;
		mono.seekg(offset);
		mono.read(mono_buf.data(), block_size);
		if (!mono) { std::printf("  hand(%2d,%2d) MONOLITH READ FAILED\n", i, j); continue; }

		std::string split_path = split_dir + "/" + std::to_string(handid) + ".bin";
		std::ifstream sf(split_path, std::ios::binary);
		if (!sf) { std::printf("  hand(%2d,%2d) SPLIT FILE MISSING: %s\n", i, j, split_path.c_str()); continue; }
		sf.read(split_buf.data(), block_size);

		bool ok = (memcmp(mono_buf.data(), split_buf.data(), block_size) == 0);
		if (ok) passed++;
		std::printf("  hand(%2d,%2d) handid=%4d  offset=%13lld  %zu bytes compared  %s\n",
			i, j, handid, offset, mono_buf.size(), ok ? "BYTE-EXACT MATCH" : "MISMATCH!!");
	}
	std::printf("\nResult: %d/%d hole-hand blocks byte-exact match between monolith and split files\n", passed, n_samples);
	return (passed == n_samples) ? 0 : 1;
}
