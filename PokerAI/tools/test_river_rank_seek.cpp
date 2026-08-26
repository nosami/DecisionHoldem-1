//###############################################################################
//   test_river_rank_seek.cpp -- validates a proposed O(1) DIRECT-SEEK lookup
//   into a per-hole-hand split river cluster file (see BUILD_NOTES.md
//   section 31), as an alternative to the ORIGINAL codebase's O(log n)
//   binary search over the full in-RAM keys[] array (Engine.h's
//   find_river()). The goal (see section 34): if a specific 5-card board's
//   row position within a hand's file can be computed directly (no full-file
//   load, no scan), a RiverClusterLeafModel for TURN mode becomes cheap
//   (a few dozen tiny seeks per hole-hand, not a 16.86GB RAM-resident
//   structure) -- reopening the idea section 29 shelved as impossible on
//   this host.
//
//   THE CLAIM BEING VALIDATED: Engine.h's key formula for a 5-card board
//   (comm[0..4] SORTED ascending by raw 0-51 card index) is
//       key = comm[0]*52^4 + comm[1]*52^3 + comm[2]*52^2 + comm[3]*52 + comm[4]
//   Sorting all C(50,5) possible boards (the 50 cards excluding this hand's
//   own 2 hole cards) by this key value is IDENTICAL to sorting them in
//   standard lexicographic tuple order (compare comm[0] first, then
//   comm[1], etc.) -- because each digit position is bounded by the same
//   base (52) regardless of the 2 missing (hole-card) values, so relative
//   order is preserved exactly as if the universe were contiguously
//   relabeled 0..49. Lexicographic-order rank of a k-subset has a standard
//   closed-form "successor counting" formula (implemented in
//   lex_rank_of_combination() below, hand-verified against a tiny n=4,k=2
//   example before ever touching real data -- see BUILD_NOTES.md).
//
//   THIS TOOL never assumes the claim is true -- it checks it, directly,
//   against the REAL split river-cluster files already validated
//   byte-exact against the monolithic source in section 31.
//
//   For N random real (hole-hand, 5-card board) pairs:
//     1. Computes the expected `key` via Engine.h's own formula.
//     2. Computes R_direct = lex_rank_of_combination(...) -- the claimed
//        O(1) row position.
//     3. Reads keys[R_direct] and values[R_direct] via a SPARSE pread() at
//        the computed byte offset (no full-file load) and confirms
//        keys[R_direct] == expected key.
//     4. Independently loads the SAME hand's full keys[]/values[] arrays
//        into RAM (safe -- one hand at a time, 12.7MB) and re-derives the
//        value via the EXACT SAME binary-search algorithm as Engine.h's
//        find_river(), to confirm the sparse direct-seek's value also
//        matches a completely independent lookup method.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -o tools/test_river_rank_seek tools/test_river_rank_seek.cpp
//   RUN (from PokerAI/):
//     ./tools/test_river_rank_seek /Users/jason/dh_local_data/river_cluster_split
//###############################################################################
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <array>
#include <algorithm>
#include <random>
#include <string>
#include <fcntl.h>
#include <unistd.h>

static const int RIVER_COMMUNITY_TOTAL = 2118760;
static const long KEYS_BYTES = (long)RIVER_COMMUNITY_TOTAL * 4;
static const long VALUES_OFFSET = KEYS_BYTES;

// --- Binomial coefficient table, n up to 52, k up to 5 (tiny, exact). ---
static uint64_t C_TABLE[53][6];
static void build_binomial_table() {
	for (int n = 0; n <= 52; n++) {
		for (int k = 0; k <= 5; k++) {
			if (k == 0) C_TABLE[n][k] = 1;
			else if (k > n) C_TABLE[n][k] = 0;
			else if (k == n) C_TABLE[n][k] = 1;
			else C_TABLE[n][k] = 0; // filled below via Pascal's rule
		}
	}
	for (int n = 1; n <= 52; n++)
		for (int k = 1; k <= 5 && k < n; k++)
			C_TABLE[n][k] = C_TABLE[n - 1][k - 1] + C_TABLE[n - 1][k];
}
static uint64_t C(int n, int k) {
	if (k < 0 || n < 0 || k > n) return 0;
	return C_TABLE[n][k];
}

// Standard lex-rank ("successor counting") of a k-combination c[0]<c[1]<...
// <c[k-1], chosen from a CONTIGUOUS universe {0,...,n-1}, in the
// enumeration order where c[0] is the outermost/slowest-varying digit and
// c[k-1] is the innermost/fastest-varying -- i.e. exactly the order
// produced by comparing tuples left-to-right (standard lexicographic
// order), NOT colex/combinatorial-number-system order (which orders by
// comparing from the largest element first -- a different, more commonly
// documented convention that does NOT match Engine.h's positional-weighted
// key here). Hand-verified against universe n=4,k=2 in BUILD_NOTES.md
// before use on real data: expected order (0,1)=0,(0,2)=1,(0,3)=2,
// (1,2)=3,(1,3)=4,(2,3)=5.
static uint64_t lex_rank_of_combination(const int* c, int k, int n) {
	uint64_t rank = 0;
	int prev = -1;
	for (int i = 0; i < k; i++) {
		for (int v = prev + 1; v < c[i]; v++)
			rank += C(n - 1 - v, k - 1 - i);
		prev = c[i];
	}
	return rank;
}

static void self_test_lex_rank() {
	// n=4, k=2 hand-derived example from BUILD_NOTES.md.
	struct Case { int c[2]; uint64_t expect; };
	Case cases[] = {
		{{0,1}, 0}, {{0,2}, 1}, {{0,3}, 2}, {{1,2}, 3}, {{1,3}, 4}, {{2,3}, 5}
	};
	for (auto& tc : cases) {
		uint64_t got = lex_rank_of_combination(tc.c, 2, 4);
		if (got != tc.expect) {
			std::fprintf(stderr, "SELF-TEST FAILED: lex_rank_of_combination(%d,%d; n=4) = %llu, expected %llu\n",
				tc.c[0], tc.c[1], (unsigned long long)got, (unsigned long long)tc.expect);
			std::exit(1);
		}
	}
	std::printf("Self-test passed: lex_rank_of_combination() matches hand-derived n=4,k=2 example.\n");
}

// Maps a RAW card index (0-51) to its rank within the 50-card universe that
// excludes the hand's own 2 hole cards (h1 < h2), preserving relative order.
static int compact_index(int raw, int h1, int h2) {
	int shift = 0;
	if (raw > h1) shift++;
	if (raw > h2) shift++;
	return raw - shift;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <river_cluster_split_dir> [num_trials]\n", argv[0]);
		return 1;
	}
	std::string dir = argv[1];
	int trials = (argc >= 3) ? std::atoi(argv[2]) : 200;

	build_binomial_table();
	self_test_lex_rank();

	std::mt19937 rng(12345);
	int pass = 0, fail = 0;

	for (int t = 0; t < trials; t++) {
		// Pick a random hole hand (h1 < h2).
		int h1, h2;
		do {
			h1 = rng() % 52;
			h2 = rng() % 52;
		} while (h1 == h2);
		if (h1 > h2) std::swap(h1, h2);
		int handid = h1 * 52 + h2;

		std::string path = dir + "/" + std::to_string(handid) + ".bin";
		int fd = open(path.c_str(), O_RDONLY);
		if (fd < 0) {
			std::fprintf(stderr, "SKIP: could not open %s (%s)\n", path.c_str(), std::strerror(errno));
			continue;
		}

		// Pick 5 random distinct board cards, none equal to h1/h2.
		std::vector<int> pool;
		for (int c = 0; c < 52; c++) if (c != h1 && c != h2) pool.push_back(c);
		std::shuffle(pool.begin(), pool.end(), rng);
		int comm[5];
		for (int i = 0; i < 5; i++) comm[i] = pool[i];
		std::sort(comm, comm + 5);

		uint64_t expected_key = (uint64_t)comm[0] * 7311616ULL + (uint64_t)comm[1] * 140608ULL
			+ (uint64_t)comm[2] * 2704ULL + (uint64_t)comm[3] * 52ULL + (uint64_t)comm[4];

		int compacted[5];
		for (int i = 0; i < 5; i++) compacted[i] = compact_index(comm[i], h1, h2);
		uint64_t R = lex_rank_of_combination(compacted, 5, 50);

		if (R >= (uint64_t)RIVER_COMMUNITY_TOTAL) {
			std::printf("FAIL hand=%d board=[%d,%d,%d,%d,%d]: computed rank %llu out of range\n",
				handid, comm[0], comm[1], comm[2], comm[3], comm[4], (unsigned long long)R);
			fail++;
			close(fd);
			continue;
		}

		// --- Direct sparse seek (the whole point: no full-file load) ---
		uint32_t key_at_R = 0;
		uint16_t val_at_R = 0;
		pread(fd, &key_at_R, 4, (long)R * 4);
		pread(fd, &val_at_R, 2, VALUES_OFFSET + (long)R * 2);

		// --- Independent cross-check: load this ONE hand's full arrays and
		//     binary-search them exactly like Engine.h's find_river(). ---
		std::vector<uint32_t> keys(RIVER_COMMUNITY_TOTAL);
		std::vector<uint16_t> vals(RIVER_COMMUNITY_TOTAL);
		lseek(fd, 0, SEEK_SET);
		ssize_t r1 = read(fd, keys.data(), KEYS_BYTES);
		ssize_t r2 = read(fd, vals.data(), (long)RIVER_COMMUNITY_TOTAL * 2);
		close(fd);
		if (r1 != KEYS_BYTES || r2 != (ssize_t)RIVER_COMMUNITY_TOTAL * 2) {
			std::printf("FAIL hand=%d: short read while loading full arrays\n", handid);
			fail++;
			continue;
		}
		int left = 0, right = RIVER_COMMUNITY_TOTAL - 1, mid_idx = -1;
		while (left <= right) {
			int mid = (left + right) / 2;
			if (keys[mid] == expected_key) { mid_idx = mid; break; }
			if (expected_key > keys[mid]) left = mid + 1; else right = mid - 1;
		}

		bool ok = true;
		if (key_at_R != expected_key) {
			std::printf("FAIL hand=%d board=[%d,%d,%d,%d,%d]: direct-seek key mismatch: "
				"keys[%llu]=%u, expected=%llu\n", handid, comm[0], comm[1], comm[2], comm[3], comm[4],
				(unsigned long long)R, key_at_R, (unsigned long long)expected_key);
			ok = false;
		}
		if (mid_idx < 0) {
			std::printf("FAIL hand=%d: binary search could not find expected_key=%llu at all "
				"(file may not contain this board, or search bug)\n", handid, (unsigned long long)expected_key);
			ok = false;
		}
		else if ((uint64_t)mid_idx != R) {
			std::printf("FAIL hand=%d: binary-search row (%d) != direct-seek row (%llu)\n",
				handid, mid_idx, (unsigned long long)R);
			ok = false;
		}
		else if (vals[mid_idx] != val_at_R) {
			std::printf("FAIL hand=%d: binary-search value (%u) != direct-seek value (%u)\n",
				handid, vals[mid_idx], val_at_R);
			ok = false;
		}

		if (ok) pass++; else fail++;
	}

	std::printf("\n%d/%d trials passed (direct-seek row/key/value exactly matches an independent "
		"full-array binary search, real per-hole-hand split data)\n", pass, pass + fail);
	return fail == 0 ? 0 : 1;
}
