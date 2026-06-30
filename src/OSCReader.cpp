#include "OSCReader.h"

#include <expat.h>
#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <iostream>
#include <string>

// ---- Parser state ----

struct ParseState {
    std::function<void(OSCChange&&)> cb;
    int64_t count = 0;

    ChangeType cur_change = ChangeType::Create;

    // Current element being built
    bool in_node     = false;
    bool in_way      = false;
    bool in_relation = false;

    NodeEntry     node;
    WayEntry      way;
    RelationEntry relation;
};

// ---- Expat callbacks ----

static void XMLCALL startElement(void* ud, const XML_Char* name,
                                  const XML_Char** attrs) {
    auto* s = static_cast<ParseState*>(ud);

    auto getAttr = [&](const char* key) -> const char* {
        for (int i = 0; attrs[i]; i += 2)
            if (strcmp(attrs[i], key) == 0) return attrs[i+1];
        return nullptr;
    };

    auto getInt64 = [&](const char* key, int64_t def = 0) -> int64_t {
        const char* v = getAttr(key);
        return v ? std::stoll(v) : def;
    };

    auto getDouble = [&](const char* key, double def = 0.0) -> double {
        const char* v = getAttr(key);
        return v ? std::stod(v) : def;
    };

    // Change type blocks
    if (strcmp(name, "create") == 0) { s->cur_change = ChangeType::Create; return; }
    if (strcmp(name, "modify") == 0) { s->cur_change = ChangeType::Modify; return; }
    if (strcmp(name, "delete") == 0) { s->cur_change = ChangeType::Delete; return; }

    // Node
    if (strcmp(name, "node") == 0) {
        s->in_node = true;
        s->node = NodeEntry{};
        s->node.id  = getInt64("id");
        s->node.lat = getDouble("lat");
        s->node.lon = getDouble("lon");
        return;
    }

    // Way
    if (strcmp(name, "way") == 0) {
        s->in_way = true;
        s->way = WayEntry{};
        s->way.id = getInt64("id");
        return;
    }

    // Relation
    if (strcmp(name, "relation") == 0) {
        s->in_relation = true;
        s->relation = RelationEntry{};
        s->relation.id = getInt64("id");
        return;
    }

    // Tag
    if (strcmp(name, "tag") == 0) {
        const char* k = getAttr("k");
        const char* v = getAttr("v");
        if (!k || !v) return;
        if (s->in_node)     s->node.tags[k]     = v;
        if (s->in_way)      s->way.tags[k]      = v;
        if (s->in_relation) s->relation.tags[k] = v;
        return;
    }

    // Way node ref
    if (strcmp(name, "nd") == 0 && s->in_way) {
        int64_t ref = getInt64("ref");
        if (ref) s->way.node_refs.push_back(ref);
        return;
    }

    // Relation member — only keep way members
    if (strcmp(name, "member") == 0 && s->in_relation) {
        const char* type = getAttr("type");
        if (type && strcmp(type, "way") == 0) {
            int64_t ref = getInt64("ref");
            const char* role_attr = getAttr("role");
            std::string role = role_attr ? role_attr : "";
            if (ref) s->relation.way_members.push_back({ref, std::move(role)});
        }
        return;
    }
}

static void XMLCALL endElement(void* ud, const XML_Char* name) {
    auto* s = static_cast<ParseState*>(ud);

    if (strcmp(name, "node") == 0 && s->in_node) {
        s->in_node = false;
        // Extract name from tags
        auto it = s->node.tags.find("name");
        if (it != s->node.tags.end()) s->node.name = it->second;
        s->cb(NodeChange{s->cur_change, std::move(s->node)});
        ++s->count;
        return;
    }

    if (strcmp(name, "way") == 0 && s->in_way) {
        s->in_way = false;
        auto it = s->way.tags.find("name");
        if (it != s->way.tags.end()) s->way.name = it->second;
        s->cb(WayChange{s->cur_change, std::move(s->way)});
        ++s->count;
        return;
    }

    if (strcmp(name, "relation") == 0 && s->in_relation) {
        s->in_relation = false;
        auto it = s->relation.tags.find("name");
        if (it != s->relation.tags.end()) s->relation.name = it->second;
        s->cb(RelationChange{s->cur_change, std::move(s->relation)});
        ++s->count;
        return;
    }
}

// ---- OSCReader ----

OSCReader::OSCReader(const std::string& filename)
    : filename_(filename) {}

OSCReader::~OSCReader() = default;

int64_t OSCReader::parse(std::function<void(OSCChange&&)> cb) {
    // Determine if gzipped
    bool is_gz = filename_.size() >= 3 &&
                 filename_.substr(filename_.size() - 3) == ".gz";

    // Open file
    gzFile gz = nullptr;
    FILE*  plain = nullptr;

    if (is_gz) {
        gz = gzopen(filename_.c_str(), "rb");
        if (!gz) throw std::runtime_error("OSCReader: cannot open " + filename_);
    } else {
        plain = fopen(filename_.c_str(), "rb");
        if (!plain) throw std::runtime_error("OSCReader: cannot open " + filename_);
    }

    // Create expat parser
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) throw std::runtime_error("OSCReader: XML_ParserCreate failed");

    ParseState state;
    state.cb = std::move(cb);

    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, startElement, endElement);

    // Feed data in chunks
    constexpr int BUF_SIZE = 65536;
    char buf[BUF_SIZE];
    bool done = false;

    while (!done) {
        int bytes_read = 0;
        if (is_gz)
            bytes_read = gzread(gz, buf, BUF_SIZE);
        else
            bytes_read = static_cast<int>(fread(buf, 1, BUF_SIZE, plain));

        done = (bytes_read < BUF_SIZE);

        if (XML_Parse(parser, buf, bytes_read, done) == XML_STATUS_ERROR) {
            std::string err = XML_ErrorString(XML_GetErrorCode(parser));
            XML_ParserFree(parser);
            if (gz) gzclose(gz);
            if (plain) fclose(plain);
            throw std::runtime_error("OSCReader: XML parse error: " + err);
        }
    }

    XML_ParserFree(parser);
    if (gz) gzclose(gz);
    if (plain) fclose(plain);

    return state.count;
}
