#pragma once
#include <string>
#include <cstdint>
#include <optional>
#include <utility>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <atomic>

/**
 * Node coordinate store with per-thread LZ4-compressed shard files.
 *
 * Node phase:  each thread appends (id, lon_m, lat_m) records to its own
 *              LZ4 frame file. No random access needed — pure sequential writes.
 *              Bitmap sidecar (uncompressed) tracks which IDs were written.
 *
 * Merge:       decompress each shard sequentially, copy records into the flat
 *              merged mmap file at offset id*16. Merged file is uncompressed
 *              for fast random access during the way phase.
 *
 * Way phase:   select() reads from the flat merged mmap — id*16 offset, no
 *              decompression needed.
 *
 * Not thread-safe across shards — each thread owns its shard exclusively.
 * select() and merge() require no locking (no concurrent writes at that point).
 */
class OSMMMap {
public:
    static void createFile(const std::string& base_path,
                           int64_t max_id,
                           int num_shards,
                           const std::string& shard_dir = ".");

    // open_shards_for_write: when false, shard files are NOT opened/truncated
    // for writing. Use this when resuming at the merge phase or later, since
    // the shard files already contain complete data from a previous run that
    // must be preserved for merge() to read.
    OSMMMap(const std::string& base_path,
            int64_t max_id,
            int num_shards,
            const std::string& shard_dir = ".",
            bool open_shards_for_write = true);
    ~OSMMMap();

    OSMMMap(const OSMMMap&) = delete;
    OSMMMap& operator=(const OSMMMap&) = delete;

    // Called during node phase by thread `shard` — sequential append, no locking
    void insert(int shard, int64_t id, double lon_m, double lat_m);

    // Called once by barrier completion — decompresses shards into merged file
    void merge();

    // Called during way phase — flat mmap random access, no locking
    std::optional<std::pair<double,double>> select(int64_t id) const;

    // Zero out a node entry (used by delta deletes)
    void remove(int64_t id);

    // Call once, after merge() completes and before the ways/relations phase
    // begins, to hint the kernel that subsequent access to the merged node
    // file will be random (disables readahead). See implementation comment
    // for why this is deferred rather than applied at construction time.
    void setRandomAccessHint();

    void flush();

public:
    struct ShardWriter {
        FILE* f = nullptr;          // compressed shard file
        void* lz4ctx = nullptr;     // LZ4F compression context
        std::vector<uint8_t> inbuf; // input accumulation buffer
        std::vector<uint8_t> outbuf;// LZ4 output buffer

        // Bitmap (uncompressed) — tracks written IDs
        int bm_fd = -1;
        size_t bm_size = 0;
        void* bm_map = nullptr;

        // Counter for periodic fsync/msync (bounds dirty-page buildup)
        int chunks_since_sync = 0;
    };

    // Merged file (uncompressed flat mmap)
    int merged_fd_ = -1;
    size_t merged_size_ = 0;
    void* merged_map_ = nullptr;
    int merged_bm_fd_ = -1;
    size_t merged_bm_size_ = 0;
    void* merged_bm_map_ = nullptr;

    int64_t max_id_;
    std::string base_path_;
    std::string shard_dir_;
    std::vector<ShardWriter> shards_;

    static std::string shardPath(const std::string& dir, int idx);
    static std::string shardBmpPath(const std::string& dir, int idx);
    void closeWriter(ShardWriter& s);

    // Merge progress: nodes written to merged file so far
    std::atomic<int64_t> merge_progress_{0};
    std::atomic<int64_t> merge_total_{0};  // set at start of merge()
public:
    int64_t mergeProgress() const { return merge_progress_.load(std::memory_order_relaxed); }
    int64_t mergeTotal()    const { return merge_total_.load(std::memory_order_relaxed); }
};
