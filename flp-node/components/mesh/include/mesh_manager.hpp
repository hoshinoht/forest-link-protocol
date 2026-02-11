#pragma once

#include "route_table.hpp"

namespace flp {

class MeshManager {
public:
    MeshManager() = default;

    void init();
    void run(); // main loop — called from FreeRTOS task

private:
    RouteTable route_table_;

    // TODO: BLE + LoRa transport handles
    // TODO: Event group / queue for incoming packets
};

} // namespace flp
