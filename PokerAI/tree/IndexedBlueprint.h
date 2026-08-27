#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace IndexedBlueprint {

constexpr uint32_t VERSION = 1;
constexpr uint32_t NO_CHILD = 0xffffffffU;
constexpr size_t HEADER_SIZE = 80;
constexpr size_t ENTRY_SIZE = 32;
constexpr uint32_t MAX_ACTIONS = 99;
constexpr uint32_t MAX_DEPTH = 64;
constexpr uint32_t MAX_DECISIONS = 10000000;
constexpr uint32_t MAX_EDGES = 50000000;
constexpr char MAGIC[8] = {'D', 'H', 'B', 'P', 'I', 'D', 'X', '1'};

inline uint32_t get_u32(const unsigned char* p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
		(uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t get_u64(const unsigned char* p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
	return v;
}

inline void put_u32(std::vector<unsigned char>& out, uint32_t v) {
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

inline void put_u64(std::vector<unsigned char>& out, uint64_t v) {
	for (int i = 0; i < 8; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

inline uint64_t checked_add(uint64_t a, uint64_t b, const char* what) {
	if (b > UINT64_MAX - a) throw std::runtime_error(std::string("IndexedBlueprint: overflow computing ") + what);
	return a + b;
}

inline uint64_t checked_mul(uint64_t a, uint64_t b, const char* what) {
	if (a != 0 && b > UINT64_MAX / a) throw std::runtime_error(std::string("IndexedBlueprint: overflow computing ") + what);
	return a * b;
}

class File {
public:
	explicit File(const std::string& path, int flags = O_RDONLY) : fd_(::open(path.c_str(), flags, 0644)), path_(path) {
		if (fd_ < 0) throw std::runtime_error("IndexedBlueprint: cannot open " + path + ": " + std::strerror(errno));
	}
	~File() { if (fd_ >= 0) ::close(fd_); }
	File(const File&) = delete;
	File& operator=(const File&) = delete;

	uint64_t size() const {
		struct stat st;
		if (::fstat(fd_, &st) != 0 || st.st_size < 0)
			throw std::runtime_error("IndexedBlueprint: fstat failed for " + path_ + ": " + std::strerror(errno));
		return static_cast<uint64_t>(st.st_size);
	}

	void read_exact(uint64_t offset, void* dst, size_t len) const {
		unsigned char* p = static_cast<unsigned char*>(dst);
		size_t done = 0;
		while (done < len) {
			if (offset + done > static_cast<uint64_t>(INT64_MAX))
				throw std::runtime_error("IndexedBlueprint: read offset exceeds off_t range");
			ssize_t n = ::pread(fd_, p + done, len - done, static_cast<off_t>(offset + done));
			if (n > 0) { done += static_cast<size_t>(n); continue; }
			if (n < 0 && errno == EINTR) continue;
			if (n == 0) throw std::runtime_error("IndexedBlueprint: unexpected EOF reading " + path_);
			throw std::runtime_error("IndexedBlueprint: pread failed for " + path_ + ": " + std::strerror(errno));
		}
	}

	int fd() const { return fd_; }

private:
	int fd_;
	std::string path_;
};

inline uint64_t fnv_update(uint64_t h, const void* data, size_t len) {
	const unsigned char* p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < len; ++i) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

inline uint64_t source_fingerprint(const File& file, uint64_t source_size) {
	uint64_t h = 1469598103934665603ULL;
	unsigned char encoded[8];
	for (int i = 0; i < 8; ++i) encoded[i] = static_cast<unsigned char>(source_size >> (8 * i));
	h = fnv_update(h, encoded, sizeof(encoded));
	if (source_size == 0) return h;
	const uint64_t chunk = std::min<uint64_t>(4096, source_size);
	const uint64_t offsets[] = {
		0, source_size / 4, source_size / 2, (source_size / 4) * 3, source_size - chunk
	};
	std::vector<unsigned char> buf(static_cast<size_t>(chunk));
	for (uint64_t raw : offsets) {
		uint64_t off = std::min(raw, source_size - chunk);
		for (int i = 0; i < 8; ++i) encoded[i] = static_cast<unsigned char>(off >> (8 * i));
		h = fnv_update(h, encoded, sizeof(encoded));
		file.read_exact(off, buf.data(), buf.size());
		h = fnv_update(h, buf.data(), buf.size());
	}
	return h;
}

struct Entry {
	uint64_t source_offset = 0;
	uint64_t payload_bytes = 0;
	uint32_t bucket_count = 0;
	uint32_t child_start = 0;
	uint32_t action_start = 0;
	uint16_t action_len = 0;
	uint16_t depth = 0;
};

struct Stats {
	uint64_t source_size = 0;
	uint64_t source_hash = 0;
	uint32_t decisions = 0;
	uint32_t chances = 0;
	uint32_t terminals = 0;
	uint32_t max_depth = 0;
	uint32_t edges = 0;
	uint32_t buckets_169 = 0;
	uint32_t buckets_50000 = 0;
	uint32_t buckets_5000 = 0;
	uint32_t buckets_1000 = 0;
};

class Builder {
public:
	explicit Builder(const std::string& source_path) : source_(source_path), source_size_(source_.size()) {}

	Stats build(const std::string& index_path) {
		entries_.clear();
		actions_.clear();
		children_.clear();
		stats_ = Stats{};
		stats_.source_size = source_size_;
		uint64_t offset = 0;
		parse(offset, 169, 0);
		if (offset != source_size_)
			throw std::runtime_error("IndexedBlueprint: parse ended at " + std::to_string(offset) +
				", source size is " + std::to_string(source_size_));
		stats_.source_hash = source_fingerprint(source_, source_size_);
		stats_.decisions = static_cast<uint32_t>(entries_.size());
		stats_.edges = static_cast<uint32_t>(children_.size());
		write_atomic(index_path);
		return stats_;
	}

private:
	uint32_t parse(uint64_t& offset, uint32_t bucket_count, uint32_t depth) {
		if (depth > MAX_DEPTH) throw std::runtime_error("IndexedBlueprint: tree depth exceeds safety bound");
		stats_.max_depth = std::max(stats_.max_depth, depth);
		if (checked_add(offset, 4, "node header") > source_size_)
			throw std::runtime_error("IndexedBlueprint: truncated node header at " + std::to_string(offset));
		unsigned char raw[4];
		source_.read_exact(offset, raw, sizeof(raw));
		int32_t len = static_cast<int32_t>(get_u32(raw));
		uint64_t node_offset = offset;
		offset += 4;
		if (len <= 0) {
			if (len != 0) throw std::runtime_error("IndexedBlueprint: negative action length");
			if (++stats_.terminals > MAX_EDGES) throw std::runtime_error("IndexedBlueprint: too many terminal nodes");
			return NO_CHILD;
		}
		if (len >= 100) {
			if (len != 50000 && len != 5000 && len != 1000)
				throw std::runtime_error("IndexedBlueprint: invalid chance fanout " + std::to_string(len));
			if (++stats_.chances > MAX_EDGES) throw std::runtime_error("IndexedBlueprint: too many chance nodes");
			return parse(offset, static_cast<uint32_t>(len), depth + 1);
		}
		if (static_cast<uint32_t>(len) > MAX_ACTIONS)
			throw std::runtime_error("IndexedBlueprint: action count exceeds safety bound");
		if (entries_.size() >= MAX_DECISIONS)
			throw std::runtime_error("IndexedBlueprint: decision count exceeds safety bound");
		if (children_.size() + static_cast<size_t>(len) > MAX_EDGES)
			throw std::runtime_error("IndexedBlueprint: edge count exceeds safety bound");

		uint64_t action_end = checked_add(offset, static_cast<uint64_t>(len), "action bytes");
		uint64_t payload = checked_mul(bucket_count, checked_mul(static_cast<uint64_t>(len), 16, "row bytes"), "payload bytes");
		uint64_t payload_end = checked_add(action_end, payload, "node payload");
		if (payload_end > source_size_)
			throw std::runtime_error("IndexedBlueprint: truncated payload at " + std::to_string(node_offset));
		std::vector<unsigned char> node_actions(static_cast<size_t>(len));
		source_.read_exact(offset, node_actions.data(), node_actions.size());
		for (unsigned char a : node_actions) {
			if (!(a == 'd' || a == 'l' || a == 'n' || a <= 80 || a == 160))
				throw std::runtime_error("IndexedBlueprint: invalid action byte");
		}

		uint32_t id = static_cast<uint32_t>(entries_.size());
		Entry e;
		e.source_offset = node_offset;
		e.payload_bytes = payload;
		e.bucket_count = bucket_count;
		e.child_start = static_cast<uint32_t>(children_.size());
		e.action_start = static_cast<uint32_t>(actions_.size());
		e.action_len = static_cast<uint16_t>(len);
		e.depth = static_cast<uint16_t>(depth);
		entries_.push_back(e);
		if (bucket_count == 169) ++stats_.buckets_169;
		else if (bucket_count == 50000) ++stats_.buckets_50000;
		else if (bucket_count == 5000) ++stats_.buckets_5000;
		else if (bucket_count == 1000) ++stats_.buckets_1000;
		else throw std::runtime_error("IndexedBlueprint: invalid decision bucket dimension");
		actions_.insert(actions_.end(), node_actions.begin(), node_actions.end());
		children_.resize(children_.size() + static_cast<size_t>(len), NO_CHILD);
		offset = payload_end;
		for (int i = 0; i < len; ++i)
			children_[e.child_start + static_cast<uint32_t>(i)] = parse(offset, bucket_count, depth + 1);
		return id;
	}

	void write_atomic(const std::string& path) {
		std::vector<unsigned char> body;
		body.reserve(entries_.size() * ENTRY_SIZE + actions_.size() + children_.size() * 4);
		for (const Entry& e : entries_) {
			put_u64(body, e.source_offset);
			put_u64(body, e.payload_bytes);
			put_u32(body, e.bucket_count);
			put_u32(body, e.child_start);
			put_u32(body, e.action_start);
			put_u32(body, uint32_t(e.action_len) | (uint32_t(e.depth) << 16));
		}
		body.insert(body.end(), actions_.begin(), actions_.end());
		for (uint32_t child : children_) put_u32(body, child);
		uint64_t body_hash = fnv_update(1469598103934665603ULL, body.data(), body.size());

		std::vector<unsigned char> header;
		header.insert(header.end(), MAGIC, MAGIC + sizeof(MAGIC));
		put_u32(header, VERSION);
		put_u32(header, static_cast<uint32_t>(HEADER_SIZE));
		put_u64(header, source_size_);
		put_u64(header, stats_.source_hash);
		put_u32(header, static_cast<uint32_t>(entries_.size()));
		put_u32(header, stats_.chances);
		put_u32(header, stats_.terminals);
		put_u32(header, stats_.max_depth);
		put_u32(header, static_cast<uint32_t>(children_.size()));
		put_u32(header, static_cast<uint32_t>(actions_.size()));
		put_u64(header, body_hash);
		put_u64(header, checked_add(HEADER_SIZE, body.size(), "index size"));
		put_u64(header, 0);
		if (header.size() != HEADER_SIZE) throw std::runtime_error("IndexedBlueprint: internal header size error");

		std::string temp = path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
		int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0) throw std::runtime_error("IndexedBlueprint: cannot create " + temp + ": " + std::strerror(errno));
		try {
			write_all(fd, header.data(), header.size());
			write_all(fd, body.data(), body.size());
			if (::fsync(fd) != 0) throw std::runtime_error("IndexedBlueprint: fsync failed: " + std::string(std::strerror(errno)));
			if (::close(fd) != 0) { fd = -1; throw std::runtime_error("IndexedBlueprint: close failed"); }
			fd = -1;
			if (::rename(temp.c_str(), path.c_str()) != 0)
				throw std::runtime_error("IndexedBlueprint: rename failed: " + std::string(std::strerror(errno)));
		} catch (...) {
			if (fd >= 0) ::close(fd);
			::unlink(temp.c_str());
			throw;
		}
	}

	static void write_all(int fd, const void* src, size_t len) {
		const unsigned char* p = static_cast<const unsigned char*>(src);
		size_t done = 0;
		while (done < len) {
			ssize_t n = ::write(fd, p + done, len - done);
			if (n > 0) { done += static_cast<size_t>(n); continue; }
			if (n < 0 && errno == EINTR) continue;
			throw std::runtime_error("IndexedBlueprint: write failed: " + std::string(std::strerror(errno)));
		}
	}

	File source_;
	uint64_t source_size_;
	Stats stats_;
	std::vector<Entry> entries_;
	std::vector<unsigned char> actions_;
	std::vector<uint32_t> children_;
};

struct NodePolicy {
	std::vector<unsigned char> actions;
	uint32_t bucket_count = 0;
	std::vector<double> probabilities; // bucket-major
};

class Reader {
public:
	Reader(const std::string& source_path, const std::string& index_path, size_t cache_limit_bytes = 32U * 1024U * 1024U)
		: source_(source_path), cache_limit_(cache_limit_bytes) {
		load_index(index_path);
		uint64_t actual_size = source_.size();
		if (actual_size != source_size_) throw std::runtime_error("IndexedBlueprint: source size does not match index fingerprint");
		if (source_fingerprint(source_, actual_size) != source_hash_)
			throw std::runtime_error("IndexedBlueprint: source sampled hash does not match index fingerprint");
	}

	uint32_t root() const { return entries_.empty() ? NO_CHILD : 0; }
	size_t node_count() const { return entries_.size(); }
	const Stats& stats() const { return stats_; }

	uint32_t find_node_by_source_offset(uint64_t offset) const {
		for (uint32_t i = 0; i < entries_.size(); ++i)
			if (entries_[i].source_offset == offset) return i;
		throw std::runtime_error("IndexedBlueprint: no decision node at source offset " + std::to_string(offset));
	}

	const Entry& entry(uint32_t node) const {
		if (node >= entries_.size()) throw std::runtime_error("IndexedBlueprint: node id out of range");
		return entries_[node];
	}

	std::vector<unsigned char> actions(uint32_t node) const {
		const Entry& e = entry(node);
		validate_source_node(e);
		return std::vector<unsigned char>(actions_.begin() + e.action_start,
			actions_.begin() + e.action_start + e.action_len);
	}

	uint32_t child(uint32_t node, unsigned char action) const {
		const Entry& e = entry(node);
		for (uint32_t i = 0; i < e.action_len; ++i)
			if (actions_[e.action_start + i] == action) return children_[e.child_start + i];
		throw std::runtime_error("IndexedBlueprint: action is absent from current node");
	}

	std::vector<double> row(uint32_t node, uint32_t bucket) const {
		const Entry& e = entry(node);
		validate_source_node(e);
		if (bucket >= e.bucket_count) throw std::runtime_error("IndexedBlueprint: bucket out of range");
		std::vector<unsigned char> raw(static_cast<size_t>(e.action_len) * sizeof(double));
		uint64_t offset = e.source_offset + 4 + e.action_len +
			static_cast<uint64_t>(bucket) * 2 * e.action_len * sizeof(double) +
			static_cast<uint64_t>(e.action_len) * sizeof(double);
		source_.read_exact(offset, raw.data(), raw.size());
		std::vector<double> result(e.action_len);
		for (uint32_t i = 0; i < e.action_len; ++i) {
			uint64_t bits = get_u64(raw.data() + i * sizeof(double));
			std::memcpy(&result[i], &bits, sizeof(double));
		}
		normalize(result);
		return result;
	}

	std::shared_ptr<const NodePolicy> all_rows(uint32_t node) {
		std::lock_guard<std::mutex> lock(cache_mutex_);
		auto found = cache_.find(node);
		if (found != cache_.end()) {
			lru_.splice(lru_.begin(), lru_, found->second.second);
			return found->second.first;
		}
		const Entry& e = entry(node);
		validate_source_node(e);
		std::vector<unsigned char> raw(static_cast<size_t>(e.payload_bytes));
		source_.read_exact(e.source_offset + 4 + e.action_len, raw.data(), raw.size());
		auto policy = std::make_shared<NodePolicy>();
		policy->actions = actions(node);
		policy->bucket_count = e.bucket_count;
		policy->probabilities.resize(static_cast<size_t>(e.bucket_count) * e.action_len);
		for (uint32_t b = 0; b < e.bucket_count; ++b) {
			std::vector<double> values(e.action_len);
			size_t avg = (static_cast<size_t>(b) * 2 + 1) * e.action_len * sizeof(double);
			for (uint32_t i = 0; i < e.action_len; ++i) {
				uint64_t bits = get_u64(raw.data() + avg + i * sizeof(double));
				std::memcpy(&values[i], &bits, sizeof(double));
			}
			normalize(values);
			std::copy(values.begin(), values.end(), policy->probabilities.begin() + static_cast<size_t>(b) * e.action_len);
		}
		size_t bytes = policy->probabilities.size() * sizeof(double) + policy->actions.size();
		while (!lru_.empty() && cache_bytes_ + bytes > cache_limit_) {
			uint32_t old = lru_.back();
			auto it = cache_.find(old);
			cache_bytes_ -= it->second.first->probabilities.size() * sizeof(double) + it->second.first->actions.size();
			cache_.erase(it);
			lru_.pop_back();
		}
		if (bytes <= cache_limit_) {
			lru_.push_front(node);
			cache_[node] = {policy, lru_.begin()};
			cache_bytes_ += bytes;
		}
		return policy;
	}

private:
	static void normalize(std::vector<double>& values) {
		double sum = 0.0;
		for (double& v : values) {
			if (!std::isfinite(v)) throw std::runtime_error("IndexedBlueprint: non-finite average strategy value");
			if (v < 0.0) v = 0.0;
			sum += v;
		}

		if (sum > 0.0) {
			for (double& v : values) v /= sum;
		} else if (!values.empty()) {
			double uniform = 1.0 / values.size();
			for (double& v : values) v = uniform;
		}
	}

	void validate_source_node(const Entry& e) const {
		unsigned char raw[4];
		source_.read_exact(e.source_offset, raw, sizeof(raw));
		if (get_u32(raw) != e.action_len)
			throw std::runtime_error("IndexedBlueprint: source topology disagrees with index");
		std::vector<unsigned char> source_actions(e.action_len);
		source_.read_exact(e.source_offset + 4, source_actions.data(), source_actions.size());
		if (!std::equal(source_actions.begin(), source_actions.end(), actions_.begin() + e.action_start))
			throw std::runtime_error("IndexedBlueprint: source actions disagree with index");
	}

	void load_index(const std::string& path) {
		File index(path);
		uint64_t size = index.size();
		if (size < HEADER_SIZE || size > 256U * 1024U * 1024U)
			throw std::runtime_error("IndexedBlueprint: invalid index size");
		std::vector<unsigned char> data(static_cast<size_t>(size));
		index.read_exact(0, data.data(), data.size());
		if (std::memcmp(data.data(), MAGIC, sizeof(MAGIC)) != 0)
			throw std::runtime_error("IndexedBlueprint: bad index magic");
		if (get_u32(data.data() + 8) != VERSION || get_u32(data.data() + 12) != HEADER_SIZE)
			throw std::runtime_error("IndexedBlueprint: unsupported index version/header");
		source_size_ = get_u64(data.data() + 16);
		source_hash_ = get_u64(data.data() + 24);
		uint32_t decisions = get_u32(data.data() + 32);
		stats_.source_size = source_size_;
		stats_.source_hash = source_hash_;
		stats_.decisions = decisions;
		stats_.chances = get_u32(data.data() + 36);
		stats_.terminals = get_u32(data.data() + 40);
		stats_.max_depth = get_u32(data.data() + 44);
		stats_.edges = get_u32(data.data() + 48);
		uint32_t action_bytes = get_u32(data.data() + 52);
		uint64_t body_hash = get_u64(data.data() + 56);
		uint64_t declared_size = get_u64(data.data() + 64);
		if (decisions == 0 || decisions > MAX_DECISIONS || stats_.edges > MAX_EDGES ||
			stats_.chances > MAX_EDGES || stats_.terminals > MAX_EDGES ||
			action_bytes != stats_.edges || stats_.max_depth > MAX_DEPTH || declared_size != size)
			throw std::runtime_error("IndexedBlueprint: invalid index header counts");
		uint64_t entries_bytes = checked_mul(decisions, ENTRY_SIZE, "index entries");
		uint64_t expected = checked_add(HEADER_SIZE, entries_bytes, "index layout");
		expected = checked_add(expected, action_bytes, "index actions");
		expected = checked_add(expected, checked_mul(stats_.edges, 4, "index children"), "index layout");
		if (expected != size) throw std::runtime_error("IndexedBlueprint: index layout size mismatch");
		if (fnv_update(1469598103934665603ULL, data.data() + HEADER_SIZE, data.size() - HEADER_SIZE) != body_hash)
			throw std::runtime_error("IndexedBlueprint: index checksum mismatch");

		entries_.resize(decisions);
		for (uint32_t i = 0; i < decisions; ++i) {
			const unsigned char* p = data.data() + HEADER_SIZE + static_cast<size_t>(i) * ENTRY_SIZE;
			Entry& e = entries_[i];
			e.source_offset = get_u64(p);
			e.payload_bytes = get_u64(p + 8);
			e.bucket_count = get_u32(p + 16);
			e.child_start = get_u32(p + 20);
			e.action_start = get_u32(p + 24);
			uint32_t packed = get_u32(p + 28);
			e.action_len = static_cast<uint16_t>(packed & 0xffff);
			e.depth = static_cast<uint16_t>(packed >> 16);
			if (e.bucket_count == 169) ++stats_.buckets_169;
			else if (e.bucket_count == 50000) ++stats_.buckets_50000;
			else if (e.bucket_count == 5000) ++stats_.buckets_5000;
			else if (e.bucket_count == 1000) ++stats_.buckets_1000;
			else throw std::runtime_error("IndexedBlueprint: invalid decision bucket dimension");
			uint64_t expected_payload = checked_mul(e.bucket_count,
				checked_mul(e.action_len, 16, "entry row"), "entry payload");
			if (e.action_len == 0 || e.action_len > MAX_ACTIONS || e.depth > MAX_DEPTH ||
				e.payload_bytes != expected_payload ||
				e.child_start > stats_.edges || e.action_start > action_bytes ||
				e.action_len > stats_.edges - e.child_start || e.action_len > action_bytes - e.action_start ||
				checked_add(checked_add(e.source_offset, 4 + e.action_len, "entry source"), e.payload_bytes, "entry source") > source_size_)
				throw std::runtime_error("IndexedBlueprint: invalid node entry " + std::to_string(i));
		}
		size_t actions_start = HEADER_SIZE + static_cast<size_t>(entries_bytes);
		actions_.assign(data.begin() + actions_start, data.begin() + actions_start + action_bytes);
		children_.resize(stats_.edges);
		const unsigned char* child_data = data.data() + actions_start + action_bytes;
		for (uint32_t i = 0; i < stats_.edges; ++i) {
			children_[i] = get_u32(child_data + static_cast<size_t>(i) * 4);
			if (children_[i] != NO_CHILD && children_[i] >= decisions)
				throw std::runtime_error("IndexedBlueprint: invalid child node id");
		}
		if (entries_[0].bucket_count != 169 || entries_[0].source_offset != 0)
			throw std::runtime_error("IndexedBlueprint: invalid root entry");
		uint64_t previous_offset = 0;
		std::vector<uint32_t> inbound(entries_.size(), 0);
		for (uint32_t i = 0; i < entries_.size(); ++i) {
			const Entry& e = entries_[i];
			if (i != 0 && e.source_offset <= previous_offset)
				throw std::runtime_error("IndexedBlueprint: node source offsets are not strictly increasing");
			previous_offset = e.source_offset;
			if (i + 1 < entries_.size() &&
				checked_add(checked_add(e.source_offset, 4 + e.action_len, "entry end"),
					e.payload_bytes, "entry end") > entries_[i + 1].source_offset)
				throw std::runtime_error("IndexedBlueprint: overlapping node source ranges");
			for (uint32_t a = 0; a < e.action_len; ++a) {
				unsigned char action = actions_[e.action_start + a];
				if (!(action == 'd' || action == 'l' || action == 'n' || action <= 80 || action == 160))
					throw std::runtime_error("IndexedBlueprint: invalid indexed action byte");
				for (uint32_t b = 0; b < a; ++b)
					if (actions_[e.action_start + b] == action)
						throw std::runtime_error("IndexedBlueprint: duplicate indexed action byte");
			}
			for (uint32_t a = 0; a < e.action_len; ++a) {
				uint32_t child_id = children_[e.child_start + a];
				if (child_id != NO_CHILD &&
					(child_id <= i || entries_[child_id].depth <= e.depth))
					throw std::runtime_error("IndexedBlueprint: invalid child topology");
				if (child_id != NO_CHILD) {
					uint32_t child_buckets = entries_[child_id].bucket_count;
					bool valid_dimension = child_buckets == e.bucket_count ||
						(e.bucket_count == 169 && child_buckets == 50000) ||
						(e.bucket_count == 50000 && child_buckets == 5000) ||
						(e.bucket_count == 5000 && child_buckets == 1000);
					if (!valid_dimension)
						throw std::runtime_error("IndexedBlueprint: invalid child bucket transition");
				}
				if (child_id != NO_CHILD && ++inbound[child_id] != 1)
					throw std::runtime_error("IndexedBlueprint: decision node has multiple parents");
			}
		}
		for (uint32_t i = 1; i < inbound.size(); ++i)
			if (inbound[i] != 1) throw std::runtime_error("IndexedBlueprint: unreachable decision node");
	}

	File source_;
	uint64_t source_size_ = 0;
	uint64_t source_hash_ = 0;
	Stats stats_;
	std::vector<Entry> entries_;
	std::vector<unsigned char> actions_;
	std::vector<uint32_t> children_;
	size_t cache_limit_;
	size_t cache_bytes_ = 0;
	std::mutex cache_mutex_;
	std::list<uint32_t> lru_;
	std::unordered_map<uint32_t,
		std::pair<std::shared_ptr<const NodePolicy>, std::list<uint32_t>::iterator>> cache_;
};

} // namespace IndexedBlueprint
