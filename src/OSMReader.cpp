#include "OSMReader.h"

#include <fileformat.pb.h>
#include <osmformat.pb.h>

#include <zlib.h>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <arpa/inet.h>  // ntohl

// toMercator and pointWKB used to be duplicated here (byte-for-byte
// identical to GeoUtils.cpp) — silently unexercised as an ODR violation
// since GeoUtils.cpp was never linked into the same binary as this file
// until osm_import picked it up. Now declared once in GeoUtils.h
// (included via OSMReader.h) and defined once in GeoUtils.cpp.

// ---- getTags: static free function ----

static Tags getTags(const google::protobuf::RepeatedPtrField<std::string>& strtable,
                    const google::protobuf::RepeatedField<uint32_t>& keys,
                    const google::protobuf::RepeatedField<uint32_t>& vals) {
    Tags tags;
    for (int i = 0; i < keys.size(); ++i) {
        tags[strtable.Get(static_cast<int>(keys.Get(i)))] =
             strtable.Get(static_cast<int>(vals.Get(i)));
    }
    return tags;
}

// ---- OSMReader ----

OSMReader::OSMReader(const std::string& filename) {
    file_.open(filename, std::ios::binary);
    if (!file_) throw std::runtime_error("Cannot open file: " + filename);
    file_.seekg(0, std::ios::end);
    file_size_ = file_.tellg();
    file_.seekg(0, std::ios::beg);
}

OSMReader::~OSMReader() = default;

std::streampos OSMReader::getPosition() {
    return file_.tellg();
}

void OSMReader::seekTo(std::streampos offset) {
    file_.seekg(offset, std::ios::beg);
    if (!file_)
        throw std::runtime_error("OSMReader: seekTo failed");
}

bool OSMReader::readBlob(std::vector<uint8_t>& uncompressed) {
    uint32_t hdr_size_be = 0;
    if (!file_.read(reinterpret_cast<char*>(&hdr_size_be), 4)) return false;
    uint32_t hdr_size = ntohl(hdr_size_be);

    std::vector<char> hdr_buf(hdr_size);
    if (!file_.read(hdr_buf.data(), hdr_size)) return false;

    OSMPBF::BlobHeader blobHeader;
    if (!blobHeader.ParseFromArray(hdr_buf.data(), hdr_size))
        throw std::runtime_error("Failed to parse BlobHeader");

    int32_t data_size = blobHeader.datasize();
    std::vector<char> blob_buf(data_size);
    if (!file_.read(blob_buf.data(), data_size)) return false;

    OSMPBF::Blob blob;
    if (!blob.ParseFromArray(blob_buf.data(), data_size))
        throw std::runtime_error("Failed to parse Blob");

    if (blobHeader.type() == "OSMHeader") {
        uncompressed.clear();
        return true;
    }

    if (blob.has_zlib_data()) {
        const std::string& zdata = blob.zlib_data();
        uLongf dest_len = blob.has_raw_size() ? static_cast<uLongf>(blob.raw_size())
                                               : static_cast<uLongf>(zdata.size() * 4);
        uncompressed.resize(dest_len);
        int ret = uncompress(uncompressed.data(), &dest_len,
                             reinterpret_cast<const Bytef*>(zdata.data()), zdata.size());
        if (ret != Z_OK) throw std::runtime_error("zlib decompression failed");
        uncompressed.resize(dest_len);
    } else if (blob.has_raw()) {
        const std::string& raw = blob.raw();
        uncompressed.assign(raw.begin(), raw.end());
    } else {
        uncompressed.clear();
    }
    return true;
}

void OSMReader::parsePrimitiveBlock(const std::vector<uint8_t>& data,
                                    std::vector<OSMEntry>& out) {
    // Use a fresh PrimitiveBlock each call — reusing and calling Clear()
    // would clobber string data that may still be referenced by entries
    // sitting in the queue (equivalent to Python's deepcopy).
    OSMPBF::PrimitiveBlock pb;
    if (!pb.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw std::runtime_error("Failed to parse PrimitiveBlock");

    const int32_t granularity = pb.granularity();
    const int64_t lat_offset  = pb.lat_offset();
    const int64_t lon_offset  = pb.lon_offset();
    const auto&   strtable    = pb.stringtable().s();

    for (const auto& group : pb.primitivegroup()) {

        // Dense nodes
        if (group.has_dense()) {
            const auto& dense = group.dense();
            int64_t cur_id = 0, cur_lat = 0, cur_lon = 0;
            int kv_idx = 0;
            const auto& kv = dense.keys_vals();

            for (int i = 0; i < dense.id_size(); ++i) {
                cur_id  += dense.id(i);
                cur_lat += dense.lat(i);
                cur_lon += dense.lon(i);

                double lat = (lat_offset + (int64_t)granularity * cur_lat) * 1e-9;
                double lon = (lon_offset + (int64_t)granularity * cur_lon) * 1e-9;

                Tags tags;
                while (kv_idx < kv.size() && kv.Get(kv_idx) != 0) {
                    tags[strtable.Get(kv.Get(kv_idx))] =
                         strtable.Get(kv.Get(kv_idx + 1));
                    kv_idx += 2;
                }
                kv_idx++; // skip 0 delimiter

                NodeEntry node;
                node.id   = cur_id;
                node.lon  = lon;
                node.lat  = lat;
                node.tags = std::move(tags);
                if (node.tags.count("name")) node.name = node.tags.at("name");
                out.emplace_back(std::move(node));
            }
        }

        // Regular nodes
        for (const auto& n : group.nodes()) {
            double lat = (lat_offset + (int64_t)granularity * n.lat()) * 1e-9;
            double lon = (lon_offset + (int64_t)granularity * n.lon()) * 1e-9;
            NodeEntry node;
            node.id   = n.id();
            node.lon  = lon;
            node.lat  = lat;
            node.tags = getTags(strtable, n.keys(), n.vals());
            if (node.tags.count("name")) node.name = node.tags.at("name");
            out.emplace_back(std::move(node));
        }

        // Ways
        for (const auto& w : group.ways()) {
            WayEntry way;
            way.id   = w.id();
            way.tags = getTags(strtable, w.keys(), w.vals());
            if (way.tags.count("name")) way.name = way.tags.at("name");
            int64_t cur_ref = 0;
            for (auto delta : w.refs()) {
                cur_ref += delta;
                way.node_refs.push_back(cur_ref);
            }
            out.emplace_back(std::move(way));
        }

        // Relations
        for (const auto& r : group.relations()) {
            RelationEntry rel;
            rel.id   = r.id();
            rel.tags = getTags(strtable, r.keys(), r.vals());
            if (rel.tags.count("name")) rel.name = rel.tags.at("name");
            int64_t cur_memid = 0;
            for (int i = 0; i < r.memids_size(); ++i) {
                cur_memid += r.memids(i);
                if (r.types(i) == OSMPBF::Relation::WAY) {
                    std::string role = (r.roles_sid(i) < strtable.size())
                        ? strtable[r.roles_sid(i)] : "";
                    rel.way_members.push_back({cur_memid, std::move(role)});
                }
            }
            out.emplace_back(std::move(rel));
        }
    }
}

bool OSMReader::next(std::vector<OSMEntry>& out) {
    out.clear();
    while (true) {
        std::vector<uint8_t> data;
        if (!readBlob(data)) return false;
        if (data.empty()) continue; // OSMHeader blob, skip
        parsePrimitiveBlock(data, out);
        return true;
    }
}
