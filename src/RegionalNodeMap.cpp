#include "RegionalNodeMap.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <algorithm>

// ---- header field byte offsets (see RegionalNodeMap.h for the layout) ----
static constexpr size_t OFF_MAGIC      = 0;   // char[8]
static constexpr size_t OFF_VERSION    = 8;   // uint32_t
static constexpr size_t OFF_COUNT      = 12;  // uint64_t
static constexpr size_t OFF_NAME       = 20;  // char[24]
static constexpr size_t OFF_BBOX       = 44;  // float[4]
static constexpr size_t OFF_CREATED_AT = 60;  // uint32_t
static constexpr size_t NAME_LEN       = 24;

// ---- Writer ----

RegionalNodeMap::Writer::Writer(const std::string& path, const std::string& region_name,
                                 const Bbox& bbox) {
    f_ = fopen(path.c_str(), "wb");
    if (!f_) throw std::runtime_error("RegionalNodeMap::Writer: cannot create " + path);

    uint8_t hdr[RegionalNodeMap::kHeaderSize] = {0};
    memcpy(hdr + OFF_MAGIC, RegionalNodeMap::kMagic, 8);
    uint32_t version = RegionalNodeMap::kVersion;
    memcpy(hdr + OFF_VERSION, &version, 4);
    uint64_t count0 = 0;
    memcpy(hdr + OFF_COUNT, &count0, 8);
    char name_buf[NAME_LEN] = {0};
    std::strncpy(name_buf, region_name.c_str(), NAME_LEN - 1);
    memcpy(hdr + OFF_NAME, name_buf, NAME_LEN);
    float bbox_f[4] = {
        static_cast<float>(bbox.min_lon), static_cast<float>(bbox.min_lat),
        static_cast<float>(bbox.max_lon), static_cast<float>(bbox.max_lat)};
    memcpy(hdr + OFF_BBOX, bbox_f, sizeof(bbox_f));
    uint32_t created_at = static_cast<uint32_t>(time(nullptr));
    memcpy(hdr + OFF_CREATED_AT, &created_at, 4);

    if (fwrite(hdr, 1, sizeof(hdr), f_) != sizeof(hdr))
        throw std::runtime_error("RegionalNodeMap::Writer: header write failed for " + path);
}

RegionalNodeMap::Writer::~Writer() {
    if (!finalized_) finalize();
}

void RegionalNodeMap::Writer::append(int64_t id, double lon_m, double lat_m) {
    uint8_t rec[RegionalNodeMap::kRecordSize];
    memcpy(rec, &id, 8);
    memcpy(rec + 8, &lon_m, 8);
    memcpy(rec + 16, &lat_m, 8);
    if (fwrite(rec, 1, sizeof(rec), f_) != sizeof(rec))
        throw std::runtime_error("RegionalNodeMap::Writer: record write failed");
    ++count_;
}

void RegionalNodeMap::Writer::finalize() {
    if (finalized_) return;
    finalized_ = true;
    if (!f_) return;
    if (fseek(f_, static_cast<long>(OFF_COUNT), SEEK_SET) == 0)
        fwrite(&count_, 1, 8, f_);
    fclose(f_);
    f_ = nullptr;
}

// ---- reader ----

RegionalNodeMap::RegionalNodeMap(const std::string& path) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("RegionalNodeMap: cannot open " + path);

    struct stat st;
    if (fstat(fd_, &st) != 0 || static_cast<size_t>(st.st_size) < kHeaderSize) {
        close(fd_);
        throw std::runtime_error("RegionalNodeMap: " + path + " too small or unreadable");
    }
    size_ = static_cast<size_t>(st.st_size);

    map_ = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (map_ == MAP_FAILED) {
        close(fd_);
        throw std::runtime_error("RegionalNodeMap: mmap failed for " + path);
    }

    const uint8_t* base = static_cast<const uint8_t*>(map_);
    if (memcmp(base + OFF_MAGIC, kMagic, 8) != 0) {
        munmap(map_, size_);
        close(fd_);
        throw std::runtime_error("RegionalNodeMap: bad magic in " + path);
    }
    uint32_t version;
    memcpy(&version, base + OFF_VERSION, 4);
    if (version != kVersion) {
        munmap(map_, size_);
        close(fd_);
        throw std::runtime_error("RegionalNodeMap: unsupported version in " + path);
    }
    memcpy(&record_count_, base + OFF_COUNT, 8);

    char name_buf[NAME_LEN + 1] = {0};
    memcpy(name_buf, base + OFF_NAME, NAME_LEN);
    region_name_ = name_buf;

    float bbox_f[4];
    memcpy(bbox_f, base + OFF_BBOX, sizeof(bbox_f));
    bbox_ = {bbox_f[0], bbox_f[1], bbox_f[2], bbox_f[3]};

    if (kHeaderSize + record_count_ * kRecordSize > size_) {
        munmap(map_, size_);
        close(fd_);
        throw std::runtime_error("RegionalNodeMap: truncated record data in " + path);
    }
}

RegionalNodeMap::~RegionalNodeMap() {
    if (map_) munmap(map_, size_);
    if (fd_ >= 0) close(fd_);
}

std::optional<std::pair<double,double>> RegionalNodeMap::select(int64_t id) const {
    const uint8_t* records = static_cast<const uint8_t*>(map_) + kHeaderSize;
    size_t lo = 0, hi = record_count_;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int64_t mid_id;
        memcpy(&mid_id, records + mid * kRecordSize, 8);
        if (mid_id < id) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= record_count_) return std::nullopt;
    const uint8_t* p = records + lo * kRecordSize;
    int64_t found_id;
    memcpy(&found_id, p, 8);
    if (found_id != id) return std::nullopt;
    double lon_m, lat_m;
    memcpy(&lon_m, p + 8, 8);
    memcpy(&lat_m, p + 16, 8);
    return std::make_pair(lon_m, lat_m);
}

// ---- merge ----

namespace {
inline void readRecord(const uint8_t* base, size_t idx, int64_t& id, double& lon_m, double& lat_m) {
    const uint8_t* p = base + idx * RegionalNodeMap::kRecordSize;
    memcpy(&id, p, 8);
    memcpy(&lon_m, p + 8, 8);
    memcpy(&lat_m, p + 16, 8);
}
} // namespace

bool RegionalNodeMap::merge(const std::string& path_a, const std::string& path_b,
                             const std::string& output_path) {
    RegionalNodeMap a(path_a);
    RegionalNodeMap b(path_b);

    const uint8_t* ra = static_cast<const uint8_t*>(a.map_) + kHeaderSize;
    const uint8_t* rb = static_cast<const uint8_t*>(b.map_) + kHeaderSize;

    Writer w(output_path, a.region_name_, a.bbox_);

    size_t i = 0, j = 0;
    while (i < a.record_count_ && j < b.record_count_) {
        int64_t ida, idb;
        double lona, lata, lonb, latb;
        readRecord(ra, i, ida, lona, lata);
        readRecord(rb, j, idb, lonb, latb);
        if (ida < idb) {
            w.append(ida, lona, lata);
            ++i;
        } else if (idb < ida) {
            w.append(idb, lonb, latb);
            ++j;
        } else {
            w.append(ida, lona, lata);  // same global id — identical coords, dedup
            ++i; ++j;
        }
    }
    while (i < a.record_count_) {
        int64_t id; double lon_m, lat_m;
        readRecord(ra, i++, id, lon_m, lat_m);
        w.append(id, lon_m, lat_m);
    }
    while (j < b.record_count_) {
        int64_t id; double lon_m, lat_m;
        readRecord(rb, j++, id, lon_m, lat_m);
        w.append(id, lon_m, lat_m);
    }

    w.finalize();
    return true;
}
