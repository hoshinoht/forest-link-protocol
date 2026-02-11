#include "mesh_manager.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mesh_mgr";

namespace flp {

void MeshManager::init()
{
    ESP_LOGI(TAG, "MeshManager initialized");
    // TODO: Initialize BLE and LoRa transports
    // TODO: Start neighbor discovery
}

void MeshManager::run()
{
    while (true) {
        // TODO: Process incoming packets from BLE/LoRa queues
        // TODO: Forward/route packets
        // TODO: Periodic neighbor table maintenance
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

} // namespace flp
