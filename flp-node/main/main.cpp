#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "flp_config.h"
#include "mesh_manager.hpp"
#include "mqtt_sn_client.hpp"
#include "protocol_selector.hpp"

static const char *TAG = "flp_main";

static flp::MeshManager mesh_manager;
static flp::MqttSnClient mqtt_client;
static flp::ProtocolSelector protocol_selector;

static void mesh_task(void *arg)
{
    ESP_LOGI(TAG, "mesh_task started");
    auto *mgr = static_cast<flp::MeshManager *>(arg);
    mgr->run();
    vTaskDelete(nullptr);
}

static void mqtt_task(void *arg)
{
    ESP_LOGI(TAG, "mqtt_task started");
    auto *client = static_cast<flp::MqttSnClient *>(arg);
    client->run();
    vTaskDelete(nullptr);
}

static void protocol_task(void *arg)
{
    ESP_LOGI(TAG, "protocol_task started");
    auto *selector = static_cast<flp::ProtocolSelector *>(arg);
    selector->run();
    vTaskDelete(nullptr);
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "FLP Node v%s starting...", FLP_VERSION);

    // Initialize NVS (required for WiFi + BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // TODO: Initialize WiFi station
    // TODO: Initialize BLE (NimBLE)

    mesh_manager.init();
    mqtt_client.init();
    protocol_selector.init();

    xTaskCreate(mesh_task, "mesh_task", FLP_MESH_TASK_STACK, &mesh_manager,
                FLP_MESH_TASK_PRIORITY, nullptr);
    xTaskCreate(mqtt_task, "mqtt_task", FLP_MQTT_TASK_STACK, &mqtt_client,
                FLP_MQTT_TASK_PRIORITY, nullptr);
    xTaskCreate(protocol_task, "protocol_task", FLP_PROTOCOL_TASK_STACK, &protocol_selector,
                FLP_PROTOCOL_TASK_PRIORITY, nullptr);

    ESP_LOGI(TAG, "All tasks created");
}
