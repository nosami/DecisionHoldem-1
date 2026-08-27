#include "../tree/IndexedBlueprint.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

namespace {

bool exists(const std::string& path) {
	struct stat st;
	return ::stat(path.c_str(), &st) == 0;
}

void require(bool value, const std::string& message) {
	if (!value) throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv) {
	std::string source = argc > 1 ? argv[1] : "/Users/jason/dh_local_data/blueprint_stgy.dat";
	std::string index = argc > 2 ? argv[2] : source + ".idx";
	if (!exists(source) || !exists(index)) {
		std::cout << "SKIP: real blueprint or sidecar index is absent\n";
		return 0;
	}
	try {
		auto open_start = std::chrono::steady_clock::now();
		IndexedBlueprint::Reader reader(source, index);
		double open_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - open_start).count();
		const auto& stats = reader.stats();
		require(stats.source_size == 16123074125ULL, "unexpected source size");
		require(stats.decisions == 118616, "unexpected decision count");
		require(stats.chances == 10864, "unexpected chance count");
		require(stats.terminals == 193774, "unexpected terminal count");
		require(stats.max_depth == 19, "unexpected max depth");
		require(stats.buckets_169 == 186, "unexpected 169-bucket node count");
		require(stats.buckets_50000 == 3264, "unexpected 50000-bucket node count");
		require(stats.buckets_5000 == 21544, "unexpected 5000-bucket node count");
		require(stats.buckets_1000 == 93622, "unexpected 1000-bucket node count");

		struct Known { uint64_t offset; uint16_t actions; const char* street; };
		const Known known[] = {
			{46001, 8, "flop"}, {12846029, 5, "turn"}, {13646051, 5, "river"}
		};
		for (const Known& item : known) {
			uint32_t node = reader.find_node_by_source_offset(item.offset);
			require(reader.entry(node).action_len == item.actions,
				std::string("wrong action count at known ") + item.street + " offset");
		}

		uint32_t flop = reader.find_node_by_source_offset(46001);
		std::vector<double> direct = reader.row(flop, 12345);
		auto payload_start = std::chrono::steady_clock::now();
		auto all = reader.all_rows(flop);
		double payload_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - payload_start).count();
		require(all->bucket_count == 50000, "known flop node has wrong bucket dimension");
		require(all->actions.size() == 8, "known flop policy has wrong action count");
		for (size_t i = 0; i < direct.size(); ++i) {
			double indexed = all->probabilities[12345 * all->actions.size() + i];
			require(std::memcmp(&direct[i], &indexed, sizeof(double)) == 0,
				"single-row and full-payload probabilities differ");
		}

		auto warm_start = std::chrono::steady_clock::now();
		auto warm = reader.all_rows(flop);
		double warm_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - warm_start).count();
		require(warm.get() == all.get(), "bounded node cache did not return cached payload");

		auto range_start = std::chrono::steady_clock::now();
		volatile double likelihood_sum = 0.0;
		for (size_t i = 0; i < 1081; ++i) {
			size_t bucket = (i * 47) % all->bucket_count;
			likelihood_sum += all->probabilities[bucket * all->actions.size()];
		}
		double range_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - range_start).count();
		require(std::isfinite(likelihood_sum), "range likelihood benchmark produced non-finite result");

		uint32_t turn = reader.find_node_by_source_offset(12846029);
		auto turn_start = std::chrono::steady_clock::now();
		auto turn_policy = reader.all_rows(turn);
		double turn_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - turn_start).count();
		require(turn_policy->bucket_count == 5000, "known turn node has wrong bucket dimension");

		std::cout << "PASS: full real index counts/known offsets/lookup equivalence\n"
			<< "reader_open_ms=" << open_ms << "\n"
			<< "flop_full_payload_ms=" << payload_ms << "\n"
			<< "flop_cached_payload_ms=" << warm_ms << "\n"
			<< "flop_1081_likelihood_rows_ms=" << range_ms << "\n"
			<< "turn_full_payload_ms=" << turn_ms << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
