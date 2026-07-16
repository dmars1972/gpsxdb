#pragma once
#include <string>
#include <cstdint>
#include <optional>
#include <utility>

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
 * to backfill the header. Reading: construct RegionalNodeMap to mmap an
 * existing file and call select(id).
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

    // Opens an existing regional file read-only and mmaps it.
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
    int fd_ = -1;
    size_t size_ = 0;
    void* map_ = nullptr;
    std::string region_name_;
    Bbox bbox_{};
    uint64_t record_count_ = 0;
};
