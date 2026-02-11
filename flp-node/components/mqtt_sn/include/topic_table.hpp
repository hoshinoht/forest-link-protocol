#pragma once

#include <cstdint>

namespace flp {

struct TopicEntry {
    uint16_t topic_id;
    char     topic_name[64];
    bool     registered;
};

class TopicTable {
public:
    TopicTable() = default;

    // TODO: Register a topic and get its ID
    uint16_t register_topic(const char *name);

    // TODO: Lookup topic name by ID
    const char *lookup(uint16_t topic_id) const;

private:
    // TODO: Synchronized topic storage
};

} // namespace flp
