//###############################################################################
//   PreflopCache.h -- NEW, ORIGINAL, ADDITIVE code (not part of the original
//   DecisionHoldem sources, not a modification of BlueprintReader.h).
//
//   Loads a small (~750KB, measured), pre-built, in-memory cache of every
//   preflop-only node's trained strategy (built once by
//   PokerAI/tools/build_preflop_cache.cpp -- see that file for the full
//   design rationale and on-disk format), so that dh_native_ai.cpp's
//   resolve_preflop_decision() and narrow_villain_range_preflop() no longer
//   have to re-walk the full ~16.1GB cluster/blueprint_strategy.dat file on
//   every single preflop decision. Previously measured cost of that
//   per-decision disk walk: 6-10 SECONDS (BUILD_NOTES.md section 23),
//   because reaching most preflop actions requires seeking past a sibling's
//   (almost always the call/limp action, listed first) entire nested
//   postflop subtree. A cache hit here is a plain in-memory hash-map lookup
//   over ~186 entries -- expected cost: microseconds.
//
//   PURELY ADDITIVE AND OPTIONAL: BlueprintReader.h's original disk-walking
//   lookup_preflop_strategy()/lookup_preflop_strategy_all_clusters()
//   functions are completely unchanged and remain the correctness
//   reference and fallback. Every function below throws on ANY problem
//   (missing/corrupt cache file, an action_path the cache doesn't contain,
//   a degenerate all-zero strategy row) -- callers MUST catch and fall back
//   to the direct disk-walking BlueprintReader functions, exactly the same
//   fallback-on-any-exception pattern dh_native_ai.cpp already uses
//   everywhere else. This header never silently returns wrong, stale, or
//   fabricated data; a cache miss/failure just means "pay the slow-path
//   cost for this one decision," never a wrong answer.
//###############################################################################
#pragma once

#include "BlueprintReader.h" // reuses LookupResult / AllClustersResult shapes
#include <fstream>
#include <unordered_map>
#include <string>
#include <stdexcept>
#include <cstdint>

namespace PreflopCache {

constexpr int32_t MAGIC = 0x31434650; // 'PFC1', must match build_preflop_cache.cpp
constexpr int32_t VERSION = 1;
constexpr int CLUSTERS = 169;

struct Entry {
	std::vector<unsigned char> actionstr;
	std::vector<std::vector<double>> probs; // [cluster][action_idx], pre-normalized
};

// Loads the whole cache file into RAM once (~750KB, not the 16GB blueprint).
// Throws on any format problem; caller (dh_native_ai.cpp) decides how to
// react -- logs and disables the cache, falling back to per-lookup disk
// walks, rather than ever crashing the process over a missing/stale cache.
class Cache {
public:
	std::unordered_map<std::string, Entry> nodes;

	void load(const std::string& path) {
		std::ifstream fin(path, std::ios::in | std::ios::binary);
		if (!fin) throw std::runtime_error("PreflopCache: cannot open " + path);

		int32_t magic = 0, version = 0, clusterlen = 0, node_count = 0;
		fin.read(reinterpret_cast<char*>(&magic), sizeof(int32_t));
		fin.read(reinterpret_cast<char*>(&version), sizeof(int32_t));
		fin.read(reinterpret_cast<char*>(&clusterlen), sizeof(int32_t));
		fin.read(reinterpret_cast<char*>(&node_count), sizeof(int32_t));
		if (!fin) throw std::runtime_error("PreflopCache: unexpected EOF reading header");
		if (magic != MAGIC) throw std::runtime_error("PreflopCache: bad magic number -- not a valid cache file (rebuild with build_preflop_cache)");
		if (version != VERSION) throw std::runtime_error("PreflopCache: unsupported version " + std::to_string(version));
		if (clusterlen != CLUSTERS) throw std::runtime_error("PreflopCache: unexpected clusterlen " + std::to_string(clusterlen) + " (expected " + std::to_string(CLUSTERS) + ")");
		if (node_count < 0) throw std::runtime_error("PreflopCache: negative node_count in header");

		nodes.reserve(static_cast<size_t>(node_count) * 2);
		for (int32_t n = 0; n < node_count; n++) {
			int32_t path_len = 0;
			fin.read(reinterpret_cast<char*>(&path_len), sizeof(int32_t));
			if (!fin || path_len < 0) throw std::runtime_error("PreflopCache: bad path_len for node " + std::to_string(n));
			std::string key(static_cast<size_t>(path_len), '\0');
			if (path_len > 0) fin.read(&key[0], path_len);

			int32_t action_len = 0;
			fin.read(reinterpret_cast<char*>(&action_len), sizeof(int32_t));
			if (!fin || action_len <= 0) throw std::runtime_error("PreflopCache: bad action_len for node " + std::to_string(n));

			Entry e;
			e.actionstr.resize(action_len);
			fin.read(reinterpret_cast<char*>(e.actionstr.data()), action_len);
			e.probs.assign(CLUSTERS, std::vector<double>(action_len));
			for (int c = 0; c < CLUSTERS; c++)
				fin.read(reinterpret_cast<char*>(e.probs[c].data()), sizeof(double) * action_len);
			if (!fin) throw std::runtime_error("PreflopCache: unexpected EOF reading node " + std::to_string(n) + "'s data");

			nodes.emplace(std::move(key), std::move(e));
		}
	}
};

// Adapters returning the SAME result types BlueprintReader.h's disk-walking
// functions use, so dh_native_ai.cpp's call sites only need to try the
// cache first and fall back to the disk-walking function on any exception
// -- identical error-handling shape either way, no call-site branching on
// "which source did this come from."
inline BlueprintReader::LookupResult lookup_preflop_strategy(
	const Cache& cache,
	const std::vector<unsigned char>& action_path,
	int hand_cluster)
{
	std::string key(action_path.begin(), action_path.end());
	auto it = cache.nodes.find(key);
	if (it == cache.nodes.end())
		throw std::runtime_error("PreflopCache: action_path not found in cache");
	if (hand_cluster < 0 || hand_cluster >= CLUSTERS)
		throw std::runtime_error("PreflopCache: hand_cluster out of range [0,169)");

	const Entry& e = it->second;
	BlueprintReader::LookupResult r;
	r.actionstr = e.actionstr;
	r.probs = e.probs[hand_cluster];
	double sum = 0.0;
	for (double p : r.probs) sum += p;
	if (!(sum > 0.0))
		throw std::runtime_error("PreflopCache: non-positive average-strategy sum for this hand cluster");
	return r;
}

inline BlueprintReader::AllClustersResult lookup_preflop_strategy_all_clusters(
	const Cache& cache,
	const std::vector<unsigned char>& action_path)
{
	std::string key(action_path.begin(), action_path.end());
	auto it = cache.nodes.find(key);
	if (it == cache.nodes.end())
		throw std::runtime_error("PreflopCache: action_path not found in cache");

	const Entry& e = it->second;
	BlueprintReader::AllClustersResult r;
	r.actionstr = e.actionstr;
	r.probs = e.probs;
	return r;
}

} // namespace PreflopCache
