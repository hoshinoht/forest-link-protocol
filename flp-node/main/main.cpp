#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "flp_config.h"
#include "mesh_manager.hpp"
#include "mqtt_sn_client.hpp"
#include "protocol_selector.hpp"

static const char *TAG = "flp_main";

static flp::MeshManager mesh_manager;
static flp::MqttSnClient mqtt_client;
static flp::ProtocolSelector protocol_selector;

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // Task 1: Wire WiFi status into MeshManager
        mesh_manager.set_has_internet(false);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // Task 1: Wire WiFi status into MeshManager
        mesh_manager.set_has_internet(true);
    }
}

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

    // Initialize WiFi station
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_FLP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_FLP_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi station initialized, connecting...");

    // BLE init is handled by BleTransport::init() called from MeshManager

    mesh_manager.set_lora_rx_priority(FLP_LORA_RX_TASK_PRIORITY);
    mesh_manager.init();

    // Task 6: Wire MQTT client to mesh manager for file upload bridge
    mqtt_client.set_node_addr(mesh_manager.get_addr());
    mqtt_client.init();
    mesh_manager.set_mqtt_client(&mqtt_client);

    protocol_selector.init();

    xTaskCreate(mesh_task, "mesh_task", FLP_MESH_TASK_STACK, &mesh_manager,
                FLP_MESH_TASK_PRIORITY, nullptr);
    xTaskCreate(mqtt_task, "mqtt_task", FLP_MQTT_TASK_STACK, &mqtt_client,
                FLP_MQTT_TASK_PRIORITY, nullptr);
    xTaskCreate(protocol_task, "protocol_task", FLP_PROTOCOL_TASK_STACK, &protocol_selector,
                FLP_PROTOCOL_TASK_PRIORITY, nullptr);

    ESP_LOGI(TAG, "All tasks created");
}
