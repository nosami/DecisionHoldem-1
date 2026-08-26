//###############################################################################
//   split_cluster_file.cpp -- one-time converter: splits a monolithic
//   per-hole-hand cluster file (turn_hand_cluster.bin / river_hand_cluster.bin
//   -- both share the same "1326 fixed-size blocks, one per hole-hand combo"
//   layout, see BUILD_NOTES.md sections 28/30) into 1326 SEPARATE FILES, one
//   per hole-hand key, named by the flat hand id `i*52+j`. This is the
//   user-proposed alternative to computing a byte offset into one big file:
//   let the filesystem's own directory/filename lookup do the indexing, so
//   loading a subset of hole-hands is just "open the files you need by name"
//   with no offset arithmetic at all.
//
//   Streams the source file sequentially (constant, small RAM use
//   regardless of the source file's total size) and writes each block to
//   its own output file as soon as it's read -- never holds more than one
//   block in memory at once.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -o tools/split_cluster_file tools/split_cluster_file.cpp
//   RUN (from PokerAI/), e.g. for turn_hand_cluster.bin (community_total=230300,
//   key_bytes=4, val_bytes=4) into a target directory:
//     ./tools/split_cluster_file <source_file> <out_dir> <community_total> <key_bytes> <val_bytes>
//   For river_hand_cluster.bin: community_total=2118760, key_bytes=4, val_bytes=2.
//###############################################################################
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>

int main(int argc, char** argv) {
	if (argc != 6) {
		std::fprintf(stderr, "usage: %s <source_file> <out_dir> <community_total> <key_bytes> <val_bytes>\n", argv[0]);
		return 1;
	}
	std::string src_path = argv[1];
	std::string out_dir = argv[2];
	long long community_total = atoll(argv[3]);
	long long key_bytes = atoll(argv[4]);
	long long val_bytes = atoll(argv[5]);
	long long block_size = community_total * (key_bytes + val_bytes);

	mkdir(out_dir.c_str(), 0755);

	std::ifstream in(src_path, std::ios::binary);
	if (!in) { std::fprintf(stderr, "cannot open source file %s\n", src_path.c_str()); return 1; }

	std::vector<char> buf(block_size);
	int n_written = 0;
	for (int i = 0; i < 51; i++) {
		for (int j = i + 1; j < 52; j++) {
			in.read(buf.data(), block_size);
			if (!in) { std::fprintf(stderr, "short read at hand (%d,%d) -- source file truncated?\n", i, j); return 1; }
			int handid = i * 52 + j;
			std::string out_path = out_dir + "/" + std::to_string(handid) + ".bin";
			std::ofstream out(out_path, std::ios::binary);
			out.write(buf.data(), block_size);
			out.close();
			n_written++;
		}
		if (i % 10 == 0) std::fprintf(stderr, "...row i=%d done (%d files written so far)\n", i, n_written);
	}
	std::fprintf(stderr, "Done: wrote %d per-hand files (%lld bytes each) to %s\n", n_written, block_size, out_dir.c_str());
	return 0;
}
