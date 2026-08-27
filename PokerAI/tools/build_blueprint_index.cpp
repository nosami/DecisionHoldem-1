#include "../tree/IndexedBlueprint.h"
#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
	const std::string source = argc > 1 ? argv[1] : "cluster/blueprint_strategy.dat";
	const std::string index = argc > 2 ? argv[2] : source + ".idx";
	auto start = std::chrono::steady_clock::now();
	try {
		IndexedBlueprint::Builder builder(source);
		IndexedBlueprint::Stats stats = builder.build(index);
		double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::cout << "source_bytes=" << stats.source_size << "\n"
			<< "decisions=" << stats.decisions << "\n"
			<< "chances=" << stats.chances << "\n"
			<< "terminals=" << stats.terminals << "\n"
			<< "edges=" << stats.edges << "\n"
			<< "max_depth=" << stats.max_depth << "\n"
			<< "bucket_nodes_169=" << stats.buckets_169 << "\n"
			<< "bucket_nodes_50000=" << stats.buckets_50000 << "\n"
			<< "bucket_nodes_5000=" << stats.buckets_5000 << "\n"
			<< "bucket_nodes_1000=" << stats.buckets_1000 << "\n"
			<< "seconds=" << seconds << "\n"
			<< "index=" << index << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "build_blueprint_index: " << e.what() << "\n";
		return 1;
	}
}
