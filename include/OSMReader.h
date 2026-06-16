#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <fstream>
#include <variant>
#include <cstdint>
#include <array>

using Tags = std::unordered_map<std::string, std::string>;

// ---- Entry Types ----

struct NodeEntry {
    int64_t id;
    std::string name;
    Tags tags;
    double lon;   // WGS84
    double lat;
    double lon_m; // Web Mercator (EPSG:3857)
    double lat_m;

    // WKB hex of POINT(lon lat) in WGS84
    std::string geog_wkb_hex;
};

struct WayEntry {
    int64_t id;
    std::string name;
    Tags tags;
    std::vector<int64_t> node_refs;
    bool is_closed = false;

    // Filled after coordinate lookup; WKB hex of Polygon or LineString
    std::string geog_wkb_hex;
};

struct RelationEntry {
    int64_t id;
    std::string name;
    Tags tags;
    // Only WAY member IDs are kept (matching Python logic)
    std::vector<int64_t> way_members;

    std::string geog_wkb_hex; // filled during processing
};

using OSMEntry = std::variant<NodeEntry, WayEntry, RelationEntry>;

// ---- Projection helpers (thread-safe via thread-local PROJ context) ----
std::pair<double,double> toMercator(double lon, double lat);
std::string pointWKB(double lon, double lat);

// ---- OSMReader ----

class OSMReader {
public:
    explicit OSMReader(const std::string& filename);
    ~OSMReader();

    // Returns false when EOF
    bool next(std::vector<OSMEntry>& out);

    std::streampos getSize() const { return file_size_; }
    std::streampos getPosition();

    // Seek to a blob-aligned byte offset (as returned by getPosition()).
    // Must be called before the first call to next().
    void seekTo(std::streampos offset);

private:
    std::ifstream file_;
    std::streampos file_size_;

    bool readBlob(std::vector<uint8_t>& uncompressed);
    void parsePrimitiveBlock(const std::vector<uint8_t>& data,
                             std::vector<OSMEntry>& out);
};
