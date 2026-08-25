"""
Empirical benchmark: binary search executed directly against the on-disk
cluster files via os.pread(), WITHOUT loading the arrays into memory.

Mirrors Engine.h's find_turn()/find_river()/find_strength() algorithms
exactly (same comparison logic, same array layout/offsets) but replaces
in-RAM array indexing with a per-comparison disk read, to measure whether
binary search directly against the file is a viable alternative to loading
the full 2-16GB cluster arrays into RAM (see BUILD_NOTES.md section 2/8).

On macOS, pass "nocache" (default) to force genuine disk I/O per read via
the F_NOCACHE fcntl (bypasses the page cache) -- otherwise the benchmark
would just measure RAM/page-cache speed on any file previously touched.
Pass "cache" to instead measure normal (page-cache-eligible) performance.
F_NOCACHE is macOS-specific; on other platforms "nocache" mode falls back
to normal reads with a warning.

Usage:
    python3 bench_disk_binary_search.py [nocache|cache] [trials]

Requires the real (byte-verified) cluster files to already be present under
PokerAI/cluster/ -- see BUILD_NOTES.md section 2 for how to obtain them.
"""
import os
import struct
import random
import sys
import time

F_NOCACHE = 48  # macOS <fcntl.h>; not defined/applicable on Linux
CLUSTER_DIR = os.path.join(os.path.dirname(__file__), "..", "cluster")


def open_file(path, nocache):
    fd = os.open(path, os.O_RDONLY)
    if nocache:
        try:
            import fcntl
            fcntl.fcntl(fd, F_NOCACHE, 1)
        except (ImportError, OSError, AttributeError):
            print("(warning: F_NOCACHE unavailable on this platform; "
                  "falling back to normal cached reads)", file=sys.stderr)
    return fd


def disk_binary_search_u32(fd, base_offset, n, target):
    """Mirrors find_turn/find_flop/find_river: sorted array of N unsigned
    (4-byte) keys, starting at base_offset. Returns (found, num_reads)."""
    left, right = 0, n - 1
    reads = 0
    while left <= right:
        middle = (left + right) // 2
        reads += 1
        key = struct.unpack('<I', os.pread(fd, 4, base_offset + middle * 4))[0]
        if key == target:
            return True, reads
        if target > key:
            left = middle + 1
        else:
            right = middle - 1
    return False, reads


def disk_binary_search_i64(fd, base_offset, n, target):
    """Mirrors find_strength: sorted array of N signed 8-byte (ll) keys."""
    left, right = 0, n - 1
    reads = 0
    while left <= right:
        middle = (left + right) // 2
        reads += 1
        key = struct.unpack('<q', os.pread(fd, 8, base_offset + middle * 8))[0]
        if key == target:
            return True, reads
        if target > key:
            left = middle + 1
        else:
            right = middle - 1
    return False, reads


def _time_lookups(fd, base_offset, n, search_fn, key_size, fmt, trials, seed):
    lo = struct.unpack(fmt, os.pread(fd, key_size, base_offset))[0]
    hi = struct.unpack(fmt, os.pread(fd, key_size, base_offset + (n - 1) * key_size))[0]
    times, total_reads = [], 0
    random.seed(seed)
    for _ in range(trials):
        target = random.randint(lo, hi)
        t0 = time.perf_counter()
        _, reads = search_fn(fd, base_offset, n, target)
        times.append(time.perf_counter() - t0)
        total_reads += reads
    return times, total_reads / trials


def bench_turn(nocache=True, trials=200):
    """Binary search inside one hand-id's own block of turn_hand_cluster.bin."""
    path = os.path.join(CLUSTER_DIR, "turn_hand_cluster.bin")
    n = 230300
    block_bytes = n * 4 * 2  # keys[n] then values[n]
    k = 660  # mid-file block, avoids unrepresentative edge-of-file caching
    fd = open_file(path, nocache)
    try:
        return _time_lookups(fd, k * block_bytes, n, disk_binary_search_u32,
                              4, '<I', trials, seed=42)
    finally:
        os.close(fd)


def bench_seven(nocache=True, trials=200):
    """Binary search across the whole (non-blocked) sevencards_strength.bin."""
    path = os.path.join(CLUSTER_DIR, "sevencards_strength.bin")
    n = 133784560
    fd = open_file(path, nocache)
    try:
        return _time_lookups(fd, 0, n, disk_binary_search_i64,
                              8, '<q', trials, seed=7)
    finally:
        os.close(fd)


def bench_river(nocache=True, trials=200):
    """Binary search inside handid=1's block of river_hand_cluster.bin.
    Accepts a partially-downloaded '.part' file -- handid=1 is the first
    block written, so it is complete as soon as the download has begun."""
    candidates = [f for f in os.listdir(CLUSTER_DIR)
                  if f.startswith("river_hand_cluster.bin")]
    if not candidates:
        return None, None
    path = os.path.join(CLUSTER_DIR, sorted(candidates)[0])
    n = 2118760
    fd = open_file(path, nocache)
    try:
        return _time_lookups(fd, 0, n, disk_binary_search_u32,
                              4, '<I', trials, seed=99)
    finally:
        os.close(fd)


def summarize(name, times, avg_reads):
    if times is None:
        print(f"--- {name}: SKIPPED (file not found) ---")
        return
    times_ms = sorted(t * 1000 for t in times)
    n = len(times_ms)
    print(f"--- {name} ---")
    print(f"trials={n}  avg_reads/search={avg_reads:.1f}")
    print(f"mean={sum(times_ms)/n:.4f} ms   median={times_ms[n//2]:.4f} ms   "
          f"p10={times_ms[n//10]:.4f} ms   p90={times_ms[9*n//10]:.4f} ms   "
          f"min={times_ms[0]:.4f} ms   max={times_ms[-1]:.4f} ms")
    print(f"implied per-read (seek) latency ~= {(sum(times_ms)/n)/avg_reads*1000:.1f} us\n")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "nocache"
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    nocache = (mode == "nocache")
    print(f"=== mode: {'F_NOCACHE (forced disk I/O)' if nocache else 'default (page cache allowed)'}, "
          f"trials={trials} ===\n")

    summarize("turn_hand_cluster.bin (N=230,300/hand, per-hand block search)",
              *bench_turn(nocache=nocache, trials=trials))
    summarize("river_hand_cluster.bin (N=2,118,760/hand, per-hand block search)",
              *bench_river(nocache=nocache, trials=trials))
    summarize("sevencards_strength.bin (N=133,784,560, whole-file search)",
              *bench_seven(nocache=nocache, trials=trials))
