#pragma once

#include "topic_table.hpp"

namespace flp {

class MqttSnClient {
public:
    MqttSnClient() = default;

    void init();
    void run(); // main loop — called from FreeRTOS task

    // TODO: Publish data to a topic
    int publish(uint16_t topic_id, const uint8_t *data, size_t len);

private:
    TopicTable topic_table_;
    // TODO: MQTT client handle, connection state
};

} // namespace flp
