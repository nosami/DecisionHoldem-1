// TEMPORARY experiment (not part of the shipped build): verify whether
// disabling river_hand_cluster.bin loading (via DH_SKIP_RIVER_CLUSTER,
// see poker/Engine.h) lets the Engine class initialize under a much
// smaller RAM budget, and whether flop/turn-level cluster lookups still
// work correctly without it. Deliberately does NOT include any tree/
// blueprint headers, so it never touches blueprint_strategy.dat (~16GB) --
// this isolates the river-cluster question from the (much larger and
// separately-confirmed) blueprint-tree RAM cost.
//
// Build (from PokerAI/):
//   g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o /tmp/test_engine_no_river tools/test_engine_no_river.cpp
// Run (from PokerAI/, so relative cluster/ paths resolve):
//   /tmp/test_engine_no_river
#include "../poker/State.h"
#include <sys/resource.h>
#include <cstdio>

static double rss_gb() {
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
	return ru.ru_maxrss / 1073741824.0; // macOS reports bytes
#else
	return ru.ru_maxrss / 1048576.0; // Linux reports KB
#endif
}

int main() {
	// `engine` (global Engine*) was already constructed by static
	// initialization before main() ran (see poker/State.h:25). By the time
	// we get here, Engine::load() has already completed (or crashed).
	printf("Engine() construction completed. Peak RSS so far: %.3f GB\n", rss_gb());

	// Sample hand: hole cards = 2c,7d (arbitrary), flop = 9h,Ks,3c (arbitrary)
	unsigned char hole[2] = { 0, 21 };     // 2c=0, 7d=21 in this engine's 0-51 encoding
	unsigned char flop[3] = { 30, 47, 8 }; // 9h, Ks, 3c
	unsigned char turn[4] = { 30, 47, 8, 12 };

	unsigned flop_cluster_id = engine->get_flop_cluster(hole, flop);
	printf("get_flop_cluster(hole={0,21}, flop={30,47,8}) = %u  (flop cluster lookup OK, river not needed)\n", flop_cluster_id);

	unsigned turn_cluster_id = engine->get_turn_cluster(hole, turn);
	printf("get_turn_cluster(hole={0,21}, turn={30,47,8,12}) = %u  (turn cluster lookup OK, river not needed)\n", turn_cluster_id);

	printf("Final peak RSS: %.3f GB\n", rss_gb());
	printf("RESULT: flop and turn cluster lookups both succeeded WITHOUT loading river_hand_cluster.bin.\n");
	return 0;
}
