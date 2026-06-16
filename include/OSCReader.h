#pragma once
#include "OSMReader.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ---- OSC Change Types ----

enum class ChangeType { Create, Modify, Delete };

struct NodeChange {
    ChangeType type;
    NodeEntry  node;
};

struct WayChange {
    ChangeType type;
    WayEntry   way;
};

struct RelationChange {
    ChangeType type;
    RelationEntry relation;
};

using OSCChange = std::variant<NodeChange, WayChange, RelationChange>;

// ---- OSCReader ----
// Parses .osc or .osc.gz files (osmChange XML format).
// Calls the provided callback for each change in document order.

class OSCReader {
public:
    explicit OSCReader(const std::string& filename);
    ~OSCReader();

    // Parse the file, invoking cb for each change.
    // Returns number of changes processed.
    int64_t parse(std::function<void(OSCChange&&)> cb);

private:
    std::string filename_;
};
