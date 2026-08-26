//###############################################################################
//   build_preflop_cache.cpp -- NEW, ORIGINAL, ADDITIVE tool (not part of the
//   original DecisionHoldem sources).
//
//   One-time (or re-run-after-blueprint-update) offline build step: walks
//   cluster/blueprint_strategy.dat (~16.1GB) ONCE, records every genuinely
//   preflop-only node's trained strategy (all 169 hand-cluster rows,
//   normalized), and writes a tiny (~750KB, measured) cache file,
//   cluster/preflop_blueprint_cache.bin, that PreflopCache.h can then load
//   into RAM in a fraction of a second at every future process start.
//
//   WHY THIS IS SAFE AND SMALL (not "load the whole 16GB into memory"):
//   State.h's legal_actions() caps preflop raises (n_raises < 2, plus
//   cur_round_action_num gating), so combined with the fixed 20000-chip
//   stack and fixed blinds, the preflop-only portion of the tree (everything
//   before a flop chance-node marker) is small and fully enumerable --
//   measured directly against the real file (read-only probe, this
//   session): 186 distinct preflop nodes, max depth 5, ~750KB total. This
//   tool records ONLY those 186 nodes' data. Every postflop subtree
//   encountered along the way is skipped (seeked past, never read into
//   memory) using the exact same skip_subtree() logic BlueprintReader.h's
//   existing, already-validated disk-walking lookups already trust today --
//   this tool does not reimplement that skip logic, it calls it directly.
//
//   HOW THIS RELATES TO BlueprintReader.h's read_node_header(): that
//   function intentionally THROWS if it ever encounters a chance-node
//   marker (action_len >= 100), because it's only ever called on paths the
//   caller already knows are still preflop. This tool, unlike any existing
//   caller, must itself decide at every branch whether a child is another
//   preflop node or a postflop chance node -- so it reads each node's
//   header directly here (mirroring read_node_header()'s own field-by-field
//   logic) rather than reusing that throwing function, and calls the
//   existing skip_subtree() only for the chance-node case.
//
//   USAGE:
//     cd PokerAI && g++ -std=c++17 -O2 -o build_preflop_cache tools/build_preflop_cache.cpp
//     ./build_preflop_cache
//   (reads cluster/blueprint_strategy.dat, writes
//   cluster/preflop_blueprint_cache.bin; prints node count/size/time)
//
//   OUTPUT FILE FORMAT (read by PreflopCache.h):
//     int32   magic       (0x31434650, i.e. bytes 'P','F','C','1')
//     int32   version     (1)
//     int32   clusterlen  (169)
//     int32   node_count
//     for each of node_count nodes, in DFS visitation order:
//       int32              path_len
//       uint8[path_len]    action_path   (action bytes from the root to this
//                                          node -- matches exactly what
//                                          dh_native_ai.cpp tracks as
//                                          LiveGame::preflop_action_path)
//       int32              action_len    (this node's own legal action count)
//       uint8[action_len]  actionstr     (this node's legal action bytes)
//       for each of 169 clusters:
//         double[action_len]  probs      (normalized to sum to 1; an
//                                          all-zero row if the original
//                                          averegret sum was non-positive
//                                          for that cluster, mirroring
//                                          BlueprintReader::AllClustersResult's
//                                          own degenerate-row handling)
//###############################################################################
#include "../tree/BlueprintReader.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <chrono>
#include <cstdint>
#include <string>

using namespace BlueprintReader;

namespace {

constexpr int CLUSTERS = 169;
constexpr int32_t MAGIC = 0x31434650; // 'PFC1'
constexpr int32_t VERSION = 1;

struct CachedNode {
	std::vector<unsigned char> action_path; // path TO this node from the root
	std::vector<unsigned char> actionstr;   // this node's own legal actions
	std::vector<std::vector<double>> probs; // [cluster][action_idx], normalized
};

std::vector<CachedNode> g_nodes;
long long g_chance_children_skipped = 0;

// Mirrors dfs_write()'s exact on-disk structure (same format
// BlueprintReader.h's read_node_header()/skip_subtree() already encode).
// Unlike read_node_header(), this does NOT throw on a chance-node marker --
// it must itself detect that case (this tool is the first caller that has
// to walk the tree without already knowing in advance which branches stay
// preflop), and delegates to the existing skip_subtree() to consume it.
void visit(std::ifstream& fin, std::vector<unsigned char>& path) {
	int32_t len;
	fin.read(reinterpret_cast<char*>(&len), sizeof(int32_t));
	if (!fin) throw std::runtime_error("build_preflop_cache: unexpected EOF reading action_len");
	if (len <= 0) return; // terminal node (fold/allin already resolved) -- no children
	if (len >= 100) {
		// Chance node (postflop deal): out of scope for the preflop cache.
		// Consume it correctly using the EXISTING, already-validated
		// skip_subtree() -- this tool never reads postflop payload data.
		g_chance_children_skipped++;
		skip_subtree(fin, len);
		return;
	}

	// A genuine preflop node.
	std::vector<unsigned char> actionstr(len);
	fin.read(reinterpret_cast<char*>(actionstr.data()), len);
	if (!fin) throw std::runtime_error("build_preflop_cache: unexpected EOF reading actionstr");

	std::vector<double> regret_scratch(len);
	std::vector<std::vector<double>> averegret(CLUSTERS, std::vector<double>(len));
	for (int c = 0; c < CLUSTERS; c++) {
		fin.read(reinterpret_cast<char*>(regret_scratch.data()), sizeof(double) * len);
		fin.read(reinterpret_cast<char*>(averegret[c].data()), sizeof(double) * len);
	}
	if (!fin) throw std::runtime_error("build_preflop_cache: unexpected EOF reading regret/averegret arrays");

	CachedNode node;
	node.action_path = path;
	node.actionstr = actionstr;
	node.probs.assign(CLUSTERS, std::vector<double>(len, 0.0));
	for (int c = 0; c < CLUSTERS; c++) {
		double sum = 0.0;
		for (int i = 0; i < len; i++)
			if (averegret[c][i] > 0) sum += averegret[c][i];
		if (sum > 0.0) {
			for (int i = 0; i < len; i++)
				node.probs[c][i] = (averegret[c][i] > 0) ? (averegret[c][i] / sum) : 0.0;
		}
		// else: leave this cluster's row all-zero, exactly mirroring
		// BlueprintReader::lookup_preflop_strategy_all_clusters()'s own
		// degenerate-row handling (never fabricates a distribution).
	}
	g_nodes.push_back(std::move(node));

	for (int i = 0; i < len; i++) {
		path.push_back(actionstr[i]);
		visit(fin, path);
		path.pop_back();
	}
}

void write_cache(const std::string& out_path) {
	std::ofstream fout(out_path, std::ios::out | std::ios::binary | std::ios::trunc);
	if (!fout) throw std::runtime_error("build_preflop_cache: cannot open " + out_path + " for writing");

	int32_t magic = MAGIC, version = VERSION, clusterlen = CLUSTERS;
	int32_t node_count = static_cast<int32_t>(g_nodes.size());
	fout.write(reinterpret_cast<const char*>(&magic), sizeof(int32_t));
	fout.write(reinterpret_cast<const char*>(&version), sizeof(int32_t));
	fout.write(reinterpret_cast<const char*>(&clusterlen), sizeof(int32_t));
	fout.write(reinterpret_cast<const char*>(&node_count), sizeof(int32_t));

	for (const auto& node : g_nodes) {
		int32_t path_len = static_cast<int32_t>(node.action_path.size());
		fout.write(reinterpret_cast<const char*>(&path_len), sizeof(int32_t));
		if (path_len > 0)
			fout.write(reinterpret_cast<const char*>(node.action_path.data()), path_len);

		int32_t action_len = static_cast<int32_t>(node.actionstr.size());
		fout.write(reinterpret_cast<const char*>(&action_len), sizeof(int32_t));
		fout.write(reinterpret_cast<const char*>(node.actionstr.data()), action_len);

		for (int c = 0; c < CLUSTERS; c++)
			fout.write(reinterpret_cast<const char*>(node.probs[c].data()), sizeof(double) * action_len);
	}
	if (!fout) throw std::runtime_error("build_preflop_cache: write failed (disk full?)");
}

} // namespace

int main(int argc, char** argv) {
	std::string in_path = (argc > 1) ? argv[1] : "cluster/blueprint_strategy.dat";
	std::string out_path = (argc > 2) ? argv[2] : "cluster/preflop_blueprint_cache.bin";

	std::ifstream fin(in_path, std::ios::in | std::ios::binary);
	if (!fin) {
		std::cerr << "build_preflop_cache: cannot open " << in_path << "\n";
		return 1;
	}

	std::cout << "Walking " << in_path << " (preflop-only region)...\n";
	auto t0 = std::chrono::steady_clock::now();
	std::vector<unsigned char> path;
	try {
		visit(fin, path);
	} catch (const std::exception& e) {
		std::cerr << "build_preflop_cache: FAILED during tree walk: " << e.what() << "\n";
		return 1;
	}
	auto t1 = std::chrono::steady_clock::now();
	double walk_secs = std::chrono::duration<double>(t1 - t0).count();

	std::cout << "  distinct preflop nodes recorded: " << g_nodes.size() << "\n";
	std::cout << "  postflop chance-node subtrees skipped: " << g_chance_children_skipped << "\n";
	std::cout << "  walk time: " << walk_secs << "s\n";

	std::cout << "Writing " << out_path << "...\n";
	auto t2 = std::chrono::steady_clock::now();
	try {
		write_cache(out_path);
	} catch (const std::exception& e) {
		std::cerr << "build_preflop_cache: FAILED writing cache: " << e.what() << "\n";
		return 1;
	}
	auto t3 = std::chrono::steady_clock::now();
	double write_secs = std::chrono::duration<double>(t3 - t2).count();

	std::ifstream check(out_path, std::ios::in | std::ios::binary | std::ios::ate);
	long long out_size = check ? static_cast<long long>(check.tellg()) : -1;

	std::cout << "  write time: " << write_secs << "s\n";
	std::cout << "  output size: " << out_size << " bytes ("
	          << (out_size / 1e6) << " MB)\n";
	std::cout << "Done. Total time: " << (walk_secs + write_secs) << "s\n";
	return 0;
}
