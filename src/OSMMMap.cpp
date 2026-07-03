#include "OSMMMap.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <stdexcept>
#include <iostream>

#include <lz4frame.h>

static constexpr size_t REC_SIZE    = 16;       // id not stored — offset IS the id
static constexpr size_t CHUNK_SIZE  = 1 << 22;  // 4 MB input chunks for LZ4 (reduced from 16MB
                                                 // to bound dirty-page buildup during node phase)

// ---- bitmap helpers ----

static inline void bitmapSet(void* bm, size_t bm_size, int64_t id) {
    size_t byte = static_cast<size_t>(id) / 8;
    if (byte >= bm_size) return;  // out of range — ignore
    static_cast<uint8_t*>(bm)[byte] |= (1u << (static_cast<size_t>(id) % 8));
}

static inline bool bitmapGet(const void* bm, size_t bm_size, int64_t id) {
    size_t byte = static_cast<size_t>(id) / 8;
    if (byte >= bm_size) return false;
    return (static_cast<const uint8_t*>(bm)[byte] >> (static_cast<size_t>(id) % 8)) & 1u;
}

// ---- path helpers ----

std::string OSMMMap::shardPath(const std::string& dir, int idx) {
    return dir + "/nodes.shard" + std::to_string(idx) + ".lz4";
}
std::string OSMMMap::shardBmpPath(const std::string& dir, int idx) {
    return dir + "/nodes.shard" + std::to_string(idx) + ".bmp";
}

// ---- mmap helpers ----

static void* openAndMap(const std::string& path, size_t sz, int& fd_out,
                        int madvise_hint = -1) {
    fd_out = open(path.c_str(), O_RDWR);
    if (fd_out < 0)
        throw std::runtime_error("OSMMMap: cannot open " + path);
    void* m = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_out, 0);
    if (m == MAP_FAILED) {
        close(fd_out);
        throw std::runtime_error("OSMMMap: mmap failed for " + path);
    }
    if (madvise_hint >= 0)
        madvise(m, sz, madvise_hint);
    return m;
}

// Grow `path` to at least `sz` bytes via ftruncate, preserving existing
// content (the file is extended with a sparse hole). No-op if the file is
// already >= sz. Used when resuming with a larger -n than the original run —
// the merged file and its bitmap must be large enough for the new max_id.
static void growFileIfNeeded(const std::string& path, size_t sz) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0)
        throw std::runtime_error("OSMMMap: cannot open " + path + " for growth check");
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        throw std::runtime_error("OSMMMap: fstat failed for " + path);
    }
    if (static_cast<size_t>(st.st_size) < sz) {
        if (ftruncate(fd, static_cast<off_t>(sz)) != 0) {
            close(fd);
            throw std::runtime_error(std::string("OSMMMap: ftruncate failed for ")
                                     + path + ": " + std::strerror(errno));
        }
        std::cerr << "[OSMMMap] grew " << path << " from "
                  << st.st_size << " to " << sz << " bytes\n";
    }
    close(fd);
}

static void createBlank(const std::string& path, size_t sz) {
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("OSMMMap: cannot create " + path);
    if (sz > 0 && ftruncate(fd, static_cast<off_t>(sz)) != 0) {
        close(fd);
        throw std::runtime_error("OSMMMap: ftruncate failed for " + path);
    }
    close(fd);
}

// ---- public API ----

void OSMMMap::createFile(const std::string& base, int64_t max_id,
                          int num_shards, const std::string& shard_dir) {
    size_t data_sz = (static_cast<size_t>(max_id) + 1) * REC_SIZE;
    size_t bm_sz   = ((static_cast<size_t>(max_id) + 1) + 7) / 8;

    // Shard files are created empty — LZ4 writes grow them
    for (int i = 0; i < num_shards; ++i) {
        createBlank(shardPath(shard_dir, i), 0);
        createBlank(shardBmpPath(shard_dir, i), 1 << 20);  // 1MB initial
    }
    // Merged flat file
    createBlank(base,          data_sz);
    createBlank(base + ".bmp", bm_sz);
}

OSMMMap::OSMMMap(const std::string& base_path, int64_t max_id,
                 int num_shards, const std::string& shard_dir,
                 bool open_shards_for_write)
    : max_id_(max_id), base_path_(base_path),
      shard_dir_(shard_dir), shards_(num_shards) {

    size_t bm_sz = ((static_cast<size_t>(max_id) + 1) + 7) / 8;

    if (open_shards_for_write) {
        // Open each shard for LZ4 writing
        for (int i = 0; i < num_shards; ++i) {
            ShardWriter& s = shards_[i];

            s.f = fopen(shardPath(shard_dir, i).c_str(), "wb");
            if (!s.f)
                throw std::runtime_error("OSMMMap: cannot open shard " + std::to_string(i));

            // Create LZ4 frame compression context
            LZ4F_errorCode_t err = LZ4F_createCompressionContext(
                reinterpret_cast<LZ4F_cctx**>(&s.lz4ctx), LZ4F_VERSION);
            if (LZ4F_isError(err))
                throw std::runtime_error(std::string("LZ4F_createCompressionContext: ")
                                         + LZ4F_getErrorName(err));

            // Write LZ4 frame header
            LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
            prefs.compressionLevel    = 9;  // LZ4 fast
            prefs.frameInfo.blockSizeID = LZ4F_max4MB;
            prefs.frameInfo.blockMode   = LZ4F_blockLinked;  // better compression across blocks

            s.outbuf.resize(LZ4F_compressBound(CHUNK_SIZE, &prefs) + LZ4F_HEADER_SIZE_MAX + 4096);
            size_t hdr_sz = LZ4F_compressBegin(
                static_cast<LZ4F_cctx*>(s.lz4ctx),
                s.outbuf.data(), s.outbuf.size(), &prefs);
            if (LZ4F_isError(hdr_sz))
                throw std::runtime_error(std::string("LZ4F_compressBegin: ")
                                         + LZ4F_getErrorName(hdr_sz));
            fwrite(s.outbuf.data(), 1, hdr_sz, s.f);

            s.inbuf.reserve(CHUNK_SIZE);

            // Open bitmap sidecar
            size_t init_bm_sz = 1 << 20;  // 1 MB initial — grows as needed
            s.bm_map = openAndMap(shardBmpPath(shard_dir, i), init_bm_sz, s.bm_fd);
            s.bm_size = init_bm_sz;
        }
    } else {
        std::cerr << "[OSMMMap] resume mode — shard files left untouched\n";
    }

    // Open merged flat file. No madvise() at construction time — see
    // setRandomAccessHint() for why this is applied later instead.
    size_t data_sz = (static_cast<size_t>(max_id) + 1) * REC_SIZE;

    // On resume, -n may be larger than the value used to create the file
    // originally. Grow the merged file and bitmap to fit, preserving
    // existing data (the extension is a sparse hole).
    if (!open_shards_for_write) {
        growFileIfNeeded(base_path, data_sz);
        growFileIfNeeded(base_path + ".bmp", bm_sz);
    }

    merged_map_    = openAndMap(base_path, data_sz, merged_fd_, -1);
    merged_size_   = data_sz;
    merged_bm_map_ = openAndMap(base_path + ".bmp", bm_sz, merged_bm_fd_);
    merged_bm_size_ = bm_sz;
}

void OSMMMap::setRandomAccessHint() {
    // MADV_RANDOM disables kernel readahead — access during the ways/relations
    // phase is effectively random (16-byte stride indexed by node id), so
    // sequential readahead just wastes I/O bandwidth on pages we won't use.
    //
    // Applied here (after merge, right before the ways phase) rather than at
    // construction time: calling madvise(MADV_RANDOM) on the full ~500GB
    // sparse mapping immediately at startup appears to trigger instability
    // on some kernels (observed full system reboots ~1.1B nodes into the
    // node phase on Ubuntu 26.04). Deferring it until the mapping is about
    // to be used for random reads avoids touching this code path during the
    // heavy sequential-write node phase.
    if (merged_map_ && merged_size_ > 0)
        madvise(merged_map_, merged_size_, MADV_RANDOM);
}

// Flush inbuf to LZ4 stream
// Periodically fsync the shard file and msync its bitmap so dirty pages
// don't accumulate unbounded across a long node-phase run. This trades a
// small amount of throughput for bounded memory pressure.
static constexpr int SYNC_EVERY_N_CHUNKS = 64; // ~256MB of input data per shard

static void flushInbuf(OSMMMap::ShardWriter& s) {
    if (s.inbuf.empty()) return;
    size_t out_sz = LZ4F_compressUpdate(
        static_cast<LZ4F_cctx*>(s.lz4ctx),
        s.outbuf.data(), s.outbuf.size(),
        s.inbuf.data(), s.inbuf.size(), nullptr);
    if (LZ4F_isError(out_sz))
        throw std::runtime_error(std::string("LZ4F_compressUpdate: ")
                                 + LZ4F_getErrorName(out_sz));
    fwrite(s.outbuf.data(), 1, out_sz, s.f);
    s.inbuf.clear();

    if (++s.chunks_since_sync >= SYNC_EVERY_N_CHUNKS) {
        fflush(s.f);
        fsync(fileno(s.f));
        if (s.bm_map && s.bm_map != MAP_FAILED)
            msync(s.bm_map, s.bm_size, MS_ASYNC);
        s.chunks_since_sync = 0;
    }
}

void OSMMMap::closeWriter(ShardWriter& s) {
    if (!s.f) return;

    // Flush remaining inbuf
    flushInbuf(s);

    // End LZ4 frame
    size_t end_sz = LZ4F_compressEnd(
        static_cast<LZ4F_cctx*>(s.lz4ctx),
        s.outbuf.data(), s.outbuf.size(), nullptr);
    if (!LZ4F_isError(end_sz))
        fwrite(s.outbuf.data(), 1, end_sz, s.f);

    LZ4F_freeCompressionContext(static_cast<LZ4F_cctx*>(s.lz4ctx));
    s.lz4ctx = nullptr;
    fclose(s.f);
    s.f = nullptr;

    if (s.bm_map && s.bm_map != MAP_FAILED) {
        msync(s.bm_map, s.bm_size, MS_SYNC);
        munmap(s.bm_map, s.bm_size);
        s.bm_map = nullptr;
    }
    if (s.bm_fd >= 0) { close(s.bm_fd); s.bm_fd = -1; }
}

OSMMMap::~OSMMMap() {
    for (auto& s : shards_) closeWriter(s);
    if (merged_map_ && merged_map_ != MAP_FAILED) {
        msync(merged_map_, merged_size_, MS_SYNC);
        munmap(merged_map_, merged_size_);
    }
    if (merged_fd_ >= 0) close(merged_fd_);
    if (merged_bm_map_ && merged_bm_map_ != MAP_FAILED) {
        msync(merged_bm_map_, merged_bm_size_, MS_SYNC);
        munmap(merged_bm_map_, merged_bm_size_);
    }
    if (merged_bm_fd_ >= 0) close(merged_bm_fd_);
}

static void growBitmap(OSMMMap::ShardWriter& s, int64_t id) {
    size_t needed = static_cast<size_t>(id) / 8 + 1;
    if (needed <= s.bm_size) return;

    // Grow by at least 25% or to needed size
    size_t new_size = std::max(needed, s.bm_size + s.bm_size / 4);

    msync(s.bm_map, s.bm_size, MS_SYNC);
    munmap(s.bm_map, s.bm_size);
    s.bm_map = nullptr;

    if (ftruncate(s.bm_fd, static_cast<off_t>(new_size)) != 0) {
        throw std::runtime_error(std::string("OSMMMap: bitmap ftruncate failed: ") + std::strerror(errno));
    }

    s.bm_map = mmap(nullptr, new_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, s.bm_fd, 0);
    if (s.bm_map == MAP_FAILED)
        throw std::runtime_error("OSMMMap: bitmap mmap failed after grow");

    s.bm_size = new_size;
}

void OSMMMap::insert(int shard, int64_t id, double lon_m, double lat_m) {
    ShardWriter& s = shards_[shard];

    // Grow bitmap if this ID is beyond current capacity
    growBitmap(s, id);

    // Pack record: id (8 bytes) + lon_m (8 bytes) + lat_m (8 bytes) = 24 bytes
    uint8_t rec[24];
    memcpy(rec,      &id,    8);
    memcpy(rec + 8,  &lon_m, 8);
    memcpy(rec + 16, &lat_m, 8);

    s.inbuf.insert(s.inbuf.end(), rec, rec + 24);
    if (s.inbuf.size() >= CHUNK_SIZE)
        flushInbuf(s);

    bitmapSet(s.bm_map, s.bm_size, id);
}

void OSMMMap::merge() {
    LZ4F_dctx* dctx = nullptr;
    LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(err))
        throw std::runtime_error(std::string("LZ4F_createDecompressionContext: ")
                                 + LZ4F_getErrorName(err));

    std::vector<uint8_t> inbuf(1 << 24);   // 16 MB read buffer
    std::vector<uint8_t> outbuf(1 << 26);  // 64 MB decompress buffer
    // Each decompressed record is 24 bytes: id + lon_m + lat_m

    // Count total nodes across all shards for progress reporting.
    // In resume mode (open_shards_for_write=false) s.bm_size is 0, so fall
    // back to max_id_ as a rough upper bound for the progress display.
    merge_total_.store(0, std::memory_order_relaxed);
    int64_t bm_total = 0;
    for (auto& s : shards_)
        bm_total += static_cast<int64_t>(s.bm_size * 8);
    merge_total_.store(bm_total > 0 ? bm_total : (max_id_ + 1), std::memory_order_relaxed);
    merge_progress_.store(0, std::memory_order_relaxed);

    // Periodically msync the merged mmap during merge so dirty pages don't
    // accumulate unbounded across potentially billions of writes — same
    // rationale as the periodic fsync added to the node-phase shard writer.
    constexpr int64_t MERGE_SYNC_EVERY_N_RECORDS = 16'000'000; // ~256MB of records
    int64_t records_since_sync = 0;

    // Lambda to safely write a record to the merged file
    auto writeToMerged = [&](int64_t id, double lon_m, double lat_m) {
        if (id < 0) {
            std::cerr << "[OSMMMap] negative node id: " << id << "\n";
            return;
        }
        size_t offset = static_cast<size_t>(id) * REC_SIZE;
        if (offset + REC_SIZE > merged_size_) {
            std::cerr << "[OSMMMap] node id " << id << " exceeds merged file size\n";
            return;
        }
        double* dst = reinterpret_cast<double*>(
            static_cast<uint8_t*>(merged_map_) + offset);
        dst[0] = lon_m;
        dst[1] = lat_m;
        bitmapSet(merged_bm_map_, merged_bm_size_, id);

        if (++records_since_sync >= MERGE_SYNC_EVERY_N_RECORDS) {
            msync(merged_map_, merged_size_, MS_ASYNC);
            msync(merged_bm_map_, merged_bm_size_, MS_ASYNC);
            records_since_sync = 0;
        }
    };

    for (int i = 0; i < static_cast<int>(shards_.size()); ++i) {
        closeWriter(shards_[i]);

        std::string path = shardPath(shard_dir_, i);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;

        // Reset decompression context for each shard
        LZ4F_resetDecompressionContext(dctx);

        size_t carry = 0; // partial record bytes carried over
        uint8_t partial[24];

        while (true) {
            size_t bytes_read = fread(inbuf.data(), 1, inbuf.size(), f);
            if (bytes_read == 0) break;

            uint8_t* src = inbuf.data();
            size_t src_rem = bytes_read;

            while (src_rem > 0) {
                size_t dst_sz = outbuf.size();
                size_t src_sz = src_rem;
                size_t ret = LZ4F_decompress(dctx, outbuf.data(), &dst_sz,
                                             src, &src_sz, nullptr);
                if (LZ4F_isError(ret))
                    throw std::runtime_error(std::string("LZ4F_decompress: ")
                                             + LZ4F_getErrorName(ret));
                src     += src_sz;
                src_rem -= src_sz;

                // Process decompressed records
                uint8_t* p   = outbuf.data();
                size_t   rem = dst_sz;

                // Handle carry from previous chunk
                if (carry > 0) {
                    size_t need = 24 - carry;
                    if (rem < need) {
                        memcpy(partial + carry, p, rem);
                        carry += rem;
                        rem = 0;
                    } else {
                        memcpy(partial + carry, p, need);
                        p   += need;
                        rem -= need;
                        carry = 0;
                        // Write partial record to merged
                        int64_t id; double lon_m, lat_m;
                        memcpy(&id,    partial,      8);
                        memcpy(&lon_m, partial + 8,  8);
                        memcpy(&lat_m, partial + 16, 8);
                        writeToMerged(id, lon_m, lat_m);
                    }
                }

                // Process full records
                while (rem >= 24) {
                    int64_t id; double lon_m, lat_m;
                    memcpy(&id,    p,      8);
                    memcpy(&lon_m, p + 8,  8);
                    memcpy(&lat_m, p + 16, 8);
                    p   += 24;
                    rem -= 24;
                    writeToMerged(id, lon_m, lat_m);
                }

                // Save partial record
                if (rem > 0) {
                    memcpy(partial, p, rem);
                    carry = rem;
                }

                if (ret == 0) break; // end of frame
            }
        }

        fclose(f);
        merge_progress_.fetch_add(static_cast<int64_t>(shards_[i].bm_size * 8),
                                  std::memory_order_relaxed);

        // Delete shard files
        unlink(path.c_str());
        unlink(shardBmpPath(shard_dir_, i).c_str());
    }

    LZ4F_freeDecompressionContext(dctx);

    // Final sync is synchronous (MS_SYNC) since the ways phase immediately
    // begins reading from merged_map_ right after this returns.
    msync(merged_map_,    merged_size_,    MS_SYNC);
    msync(merged_bm_map_, merged_bm_size_, MS_SYNC);
}

std::optional<std::pair<double,double>> OSMMMap::select(int64_t id) const {
    if (!bitmapGet(merged_bm_map_, merged_bm_size_, id)) return std::nullopt;
    size_t offset = static_cast<size_t>(id) * REC_SIZE;
    if (offset + REC_SIZE > merged_size_) return std::nullopt;
    const double* ptr = reinterpret_cast<const double*>(
        static_cast<const uint8_t*>(merged_map_) + offset);
    return std::make_pair(ptr[0], ptr[1]);
}

void OSMMMap::flush() {
    for (auto& s : shards_)
        if (s.bm_map) msync(s.bm_map, s.bm_size, MS_ASYNC);
    if (merged_map_) msync(merged_map_, merged_size_, MS_ASYNC);
}

// Direct write to the merged flat file — used by delta mode where shards
// are not available. Equivalent to insert() but bypasses shard buffering
// and writes the coordinate directly into the merged mmap.
void OSMMMap::update(int64_t id, double lon_m, double lat_m) {
    if (id < 0) return;
    size_t offset = static_cast<size_t>(id) * REC_SIZE;
    if (offset + REC_SIZE > merged_size_) return;
    double* dst = reinterpret_cast<double*>(
        static_cast<uint8_t*>(merged_map_) + offset);
    dst[0] = lon_m;
    dst[1] = lat_m;
    bitmapSet(merged_bm_map_, merged_bm_size_, id);
}

void OSMMMap::remove(int64_t id) {
    if (id < 0) return;
    size_t offset = static_cast<size_t>(id) * REC_SIZE;
    if (offset + REC_SIZE > merged_size_) return;
    // Zero out the entry and clear bitmap bit
    memset(static_cast<uint8_t*>(merged_map_) + offset, 0, REC_SIZE);
    size_t byte = static_cast<size_t>(id) / 8;
    if (byte < merged_bm_size_)
        static_cast<uint8_t*>(merged_bm_map_)[byte] &=
            ~(1u << (static_cast<size_t>(id) % 8));
}
