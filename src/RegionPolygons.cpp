#include "RegionPolygons.h"
#include "Regions.h"

#include <boost/geometry/io/wkt/read.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace RegionPolygons {

MultiPolygon parse(const std::string& wkt, const std::string& context) {
    MultiPolygon mp;
    try {
        boost::geometry::read_wkt(wkt, mp);
    } catch (const std::exception& e) {
        std::string where = context.empty() ? "" : (context + ": ");
        throw std::runtime_error("RegionPolygons::parse: " + where + "failed to parse WKT: " + e.what());
    }
    return mp;
}

std::unordered_map<std::string, MultiPolygon> load(const std::string& dir,
                                                     const std::vector<std::string>& only) {
    std::unordered_map<std::string, MultiPolygon> result;
    for (const auto& r : allExportRegions()) {
        if (!only.empty() && std::find(only.begin(), only.end(), r.name) == only.end())
            continue;

        std::string path = dir + "/" + r.name + ".wkt";
        std::ifstream in(path);
        if (!in) throw std::runtime_error("RegionPolygons::load: cannot open " + path);
        std::stringstream ss;
        ss << in.rdbuf();

        result.emplace(r.name, parse(ss.str(), path));
    }
    return result;
}

} // namespace RegionPolygons
