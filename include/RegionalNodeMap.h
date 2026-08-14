#pragma once
#include <string>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

/**
 * Sorted, ID-keyed node coordinate store for a single region — the regional
 * counterpart to OSMMMap's direct-addressed global nodes.dat.
 *
 * Unlike OSMMMap (offset = id*16 over the full global id space), a regional
 * file only holds the nodes that actually fall within one region, so it
 * stores the id explicitly and looks records up by binary search over a
 * sorted array. Growing a customer's installed file with a second region is
 * then a streaming sorted merge (see merge()), not a rebuild.
 *
 * On-disk layout:
 *   [Header, 64 bytes]
 *     char     magic[8]        = "GPSXRNM1"
 *     uint32_t version         = 1
 *     uint64_t record_count
 *     char     region_name[24] (null-padded, truncated if longer)
 *     float    bbox[4]         min_lon, min_lat, max_lon, max_lat (WGS84 deg)
 *     uint32_t created_at      (unix seconds)
 *     [padding to 64 bytes]
 *   [Records, 24 bytes each, ascending by id]
 *     int64_t  id
 *     double   lon_m
 *     double   lat_m
 *
 * Writing: use Writer to append records in ascending-id order (the natural
 * output order of OSMMMap::forEachPopulated + a bbox test) and finalize()
 * to backfill the header. Reading: construct RegionalNodeMap to read an
 * existing file and call select(id).
 *
 * On POSIX, the read path is mmap-backed (same technique as OSMMMap),
 * letting the OS page in only what's touched -- confirmed necessary, not
 * just theoretical: north_america's regional file is 66GB, which crashed
 * with std::bad_alloc on a 15GB-RAM Raspberry Pi 5 under the previous
 * plain-owned-buffer design (the original assumption here was "a few GB at
 * most," which held for smaller regions but not continent-scale ones on
 * RAM-constrained target hardware -- exactly the Pi deployment this
 * project targets). Windows keeps the original owned-buffer/ifstream
 * approach (no mmap implementation added there yet -- CreateFileMapping/
 * MapViewOfFile would be needed; large-region installs on Windows remain
 * an open follow-up, not fixed by this change).
 */
class RegionalNodeMap {
public:
    static constexpr size_t kHeaderSize = 64;
    static constexpr size_t kRecordSize = 24;  // int64 id + double lon_m + double lat_m
    static constexpr char kMagic[9] = "GPSXRNM1";
    static constexpr uint32_t kVersion = 1;

    struct Bbox { double min_lon, min_lat, max_lon, max_lat; };

    // Streaming sequential writer — caller must append in ascending id order.
    class Writer {
    public:
        Writer(const std::string& path, const std::string& region_name, const Bbox& bbox);
        ~Writer();

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        void append(int64_t id, double lon_m, double lat_m);

        // Backfills record_count into the header and closes the file. Must
        // be called for the output to be valid; the destructor calls it
        // automatically if not already done.
        void finalize();

    private:
        FILE* f_ = nullptr;
        uint64_t count_ = 0;
        bool finalized_ = false;
    };

    // Opens an existing regional file (mmap on POSIX, read into memory on
    // Windows -- see class comment).
    explicit RegionalNodeMap(const std::string& path);
    ~RegionalNodeMap();

    RegionalNodeMap(const RegionalNodeMap&) = delete;
    RegionalNodeMap& operator=(const RegionalNodeMap&) = delete;

    std::optional<std::pair<double,double>> select(int64_t id) const;

    const std::string& regionName() const { return region_name_; }
    const Bbox& bbox() const { return bbox_; }
    uint64_t recordCount() const { return record_count_; }

    // Streaming sorted-dedup merge of two regional files (order of a/b does
    // not matter) into a new output file. Used to add a region to an
    // already-installed regional node file. Output's region_name/bbox come
    // from `a`; caller is responsible for renaming output_path over the
    // target path atomically once merge() returns true.
    static bool merge(const std::string& path_a, const std::string& path_b,
                       const std::string& output_path);

private:
    // Parses the 64-byte header at `base` (magic/version/count/name/bbox),
    // validates it against `total_size`, and sets region_name_/bbox_/
    // record_count_. Shared by both platform branches of the constructor.
    void parseHeader(const std::string& path, const uint8_t* base, size_t total_size);
    const uint8_t* data() const;

#ifdef _WIN32
    std::vector<uint8_t> buf_;
#else
    void* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    int fd_ = -1;
#endif
    std::string region_name_;
    Bbox bbox_{};
    uint64_t record_count_ = 0;
};
