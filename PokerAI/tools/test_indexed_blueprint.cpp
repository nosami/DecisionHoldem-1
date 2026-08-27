#include "../tree/IndexedBlueprint.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void write_i32(std::ofstream& out, int32_t value) {
	unsigned char p[4] = {
		static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
		static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)
	};
	out.write(reinterpret_cast<const char*>(p), sizeof(p));
}

void write_decision(std::ofstream& out, int buckets, const std::vector<unsigned char>& actions,
	const std::vector<std::vector<double>>& averages) {
	write_i32(out, static_cast<int32_t>(actions.size()));
	out.write(reinterpret_cast<const char*>(actions.data()), actions.size());
	std::vector<double> zero(actions.size(), 0.0);
	for (int b = 0; b < buckets; ++b) {
		out.write(reinterpret_cast<const char*>(zero.data()), zero.size() * sizeof(double));
		out.write(reinterpret_cast<const char*>(averages[b].data()), averages[b].size() * sizeof(double));
	}
}

std::string temp_path(const char* suffix) {
	return "/tmp/dh_indexed_blueprint_" + std::to_string(static_cast<long long>(::getpid())) + suffix;
}

bool throws(const std::function<void()>& fn) {
	try { fn(); } catch (const std::exception&) { return true; }
	return false;
}

} // namespace

int main() {
	const std::string source = temp_path(".dat");
	const std::string index = temp_path(".idx");
	{
		std::ofstream out(source, std::ios::binary | std::ios::trunc);
		std::vector<std::vector<double>> root(169, {2.0, -1.0});
		root[1] = {0.0, 0.0};
		write_decision(out, 169, {'l', 2}, root);
		write_i32(out, 50000);
		std::vector<std::vector<double>> flop(50000, {1.0});
		write_decision(out, 50000, {'l'}, flop);
		write_i32(out, 0);
		write_i32(out, 0);
	}

	IndexedBlueprint::Builder builder(source);
	IndexedBlueprint::Stats built = builder.build(index);
	assert(built.decisions == 2);
	assert(built.chances == 1);
	assert(built.terminals == 2);
	assert(built.max_depth == 3);

	IndexedBlueprint::Reader reader(source, index, 64 * 1024);
	assert(reader.root() == 0);
	assert(reader.actions(0) == std::vector<unsigned char>({'l', 2}));
	assert(reader.child(0, 'l') == 1);
	assert(reader.child(0, 2) == IndexedBlueprint::NO_CHILD);
	assert(reader.entry(1).bucket_count == 50000);
	assert(reader.row(0, 0) == std::vector<double>({1.0, 0.0}));
	assert(reader.row(0, 1) == std::vector<double>({0.5, 0.5}));
	auto all = reader.all_rows(0);
	assert(all->bucket_count == 169);
	assert(all->probabilities[0] == 1.0 && all->probabilities[1] == 0.0);
	assert(reader.all_rows(0).get() == all.get());
	assert(throws([&] { reader.child(0, 'n'); }));
	assert(throws([&] { reader.row(1, 50000); }));

	const std::string truncated = temp_path(".truncated");
	{
		std::ifstream in(source, std::ios::binary);
		std::ofstream out(truncated, std::ios::binary | std::ios::trunc);
		std::vector<char> bytes(12);
		in.read(bytes.data(), bytes.size());
		out.write(bytes.data(), bytes.size());
	}
	assert(throws([&] { IndexedBlueprint::Builder bad(truncated); bad.build(temp_path(".badidx")); }));

	{
		std::fstream io(index, std::ios::binary | std::ios::in | std::ios::out);
		io.seekg(IndexedBlueprint::HEADER_SIZE + 3);
		char byte;
		io.read(&byte, 1);
		io.seekp(IndexedBlueprint::HEADER_SIZE + 3);
		byte ^= 1;
		io.write(&byte, 1);
	}
	assert(throws([&] { IndexedBlueprint::Reader bad(source, index); }));

	builder.build(index);
	{
		std::ofstream out(source, std::ios::binary | std::ios::app);
		out.put('\0');
	}
	assert(throws([&] { IndexedBlueprint::Reader stale(source, index); }));

	std::remove(source.c_str());
	std::remove(index.c_str());
	std::remove(truncated.c_str());
	std::remove(temp_path(".badidx").c_str());
	std::cout << "PASS: indexed blueprint synthetic traversal/corruption tests\n";
	return 0;
}
