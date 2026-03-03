#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "flp_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mesh_manager.hpp"
#include "nvs_flash.h"
#include "uart_ingest.hpp"
#if !CONFIG_FLP_WIFI_DISABLED
#include "mqtt_client.hpp"
#endif

#if CONFIG_FLP_OLED_ENABLED
#include "oled_display.hpp"
#endif

static const char *TAG = "flp_main";

static flp::MeshManager mesh_manager;
static flp::UartIngest uart_ingest;

#if !CONFIG_FLP_WIFI_DISABLED
static flp::MqttClient mqtt_client;
static EventGroupHandle_t s_wifi_event_group;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
#endif

#if CONFIG_FLP_OLED_ENABLED
static flp::OledDisplay oled_display;
#endif

// Demo button
static TaskHandle_t s_button_task_handle = nullptr;
static TickType_t s_last_button_press = 0;

static const uint8_t DEMO_PAYLOAD[] = "Hello from Forest Link Protocol!";

static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(s_button_task_handle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void button_task(void *arg)
{
    auto *mgr = static_cast<flp::MeshManager *>(arg);
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_button_press) < pdMS_TO_TICKS(FLP_BUTTON_DEBOUNCE_MS))
        {
            continue;
        }
        s_last_button_press = now;

        ESP_LOGI(
            TAG, "Demo transfer: demo.txt (%u bytes)", sizeof(DEMO_PAYLOAD));
        mgr->start_file_transfer(
            "demo.txt", DEMO_PAYLOAD, sizeof(DEMO_PAYLOAD));
    }
}

#if !CONFIG_FLP_WIFI_DISABLED
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // Task 1: Wire WiFi status into MeshManager
        mesh_manager.set_has_internet(false);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // Task 1: Wire WiFi status into MeshManager
        mesh_manager.set_has_internet(true);
        // Update ESP-NOW broadcast peer after WiFi connects
        mesh_manager.update_espnow_broadcast_peer();
    }
}
#endif

static void mesh_task(void *arg)
{
    ESP_LOGI(TAG, "mesh_task started");
    auto *mgr = static_cast<flp::MeshManager *>(arg);
    mgr->run();
    vTaskDelete(nullptr);
}

#if !CONFIG_FLP_WIFI_DISABLED
static void mqtt_task(void *arg)
{
    ESP_LOGI(TAG, "mqtt_task started");
    auto *client = static_cast<flp::MqttClient *>(arg);
    client->run();
    vTaskDelete(nullptr);
}
#endif

static void uart_ingest_task(void *arg)
{
    ESP_LOGI(TAG, "uart_ingest_task started");
    auto *ingest = static_cast<flp::UartIngest *>(arg);
    ingest->run();
    vTaskDelete(nullptr);
}

#if CONFIG_FLP_OLED_ENABLED
static void display_task(void *arg)
{
    ESP_LOGI(TAG, "display_task started");
    auto *mgr = static_cast<flp::MeshManager *>(arg);
    TickType_t last_wake = xTaskGetTickCount();

    while (true)
    {
        flp::NodeStatus status = {};
        status.node_addr = mgr->get_addr();
        status.wifi_connected = mgr->has_internet();
        status.espnow_peers = mgr->get_espnow_peer_count();
        status.neighbor_count = mgr->get_neighbor_count();
        status.hops_to_internet = mgr->get_hops_to_internet();
        status.transfer_active = mgr->is_transfer_active();
        status.filename = mgr->get_transfer_filename();
        status.transfer_pct = mgr->get_transfer_progress();
        status.free_heap_kb = esp_get_free_heap_size() / 1024;
        status.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

        oled_display.update(status);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FLP_DISPLAY_UPDATE_MS));
    }
}
#endif

extern "C" void app_main()
{
    ESP_LOGI(TAG, "FLP Node v%s starting...", FLP_VERSION);

    // Initialize NVS (required for WiFi + ESP-NOW)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if CONFIG_FLP_WIFI_DISABLED
    // Relay-only node: WiFi started in STA mode (no AP connect) for ESP-NOW
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t relay_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&relay_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set fixed channel for relay nodes (must match exit node AP channel)
    ESP_ERROR_CHECK(
        esp_wifi_set_channel(CONFIG_FLP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG,
             "WiFi STA started (no AP) for ESP-NOW, ch=%d",
             CONFIG_FLP_ESPNOW_CHANNEL);
#else
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
    strncpy((char *) wifi_config.sta.ssid,
            CONFIG_FLP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strncpy((char *) wifi_config.sta.password,
            CONFIG_FLP_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi station initialized, connecting...");
#endif

    // ESP-NOW init is handled by EspNowTransport::init() called from
    // MeshManager

    // Init OLED early — it's local hardware, no network dependency
#if CONFIG_FLP_OLED_ENABLED
    oled_display.init(
        CONFIG_FLP_OLED_SDA, CONFIG_FLP_OLED_SCL, CONFIG_FLP_OLED_RST);
#endif

    mesh_manager.set_lora_rx_priority(FLP_LORA_RX_TASK_PRIORITY);
    mesh_manager.init();
    mesh_manager.subscribe_topic("config");
    mesh_manager.subscribe_topic("alert");

#if !CONFIG_FLP_WIFI_DISABLED
    // Task 6: Wire MQTT client to mesh manager for file upload bridge
    mqtt_client.set_node_addr(mesh_manager.get_addr());
    mqtt_client.init();
    mesh_manager.set_mqtt_client(&mqtt_client);
#endif

    // UART ingest API
    uart_ingest.init(UART_NUM_2,
                     CONFIG_FLP_UART_TX_PIN,
                     CONFIG_FLP_UART_RX_PIN,
                     &mesh_manager);

    xTaskCreate(mesh_task,
                "mesh_task",
                FLP_MESH_TASK_STACK,
                &mesh_manager,
                FLP_MESH_TASK_PRIORITY,
                nullptr);
#if !CONFIG_FLP_WIFI_DISABLED
    xTaskCreate(mqtt_task,
                "mqtt_task",
                FLP_MQTT_TASK_STACK,
                &mqtt_client,
                FLP_MQTT_TASK_PRIORITY,
                nullptr);
#endif
    xTaskCreate(uart_ingest_task,
                "uart_ingest",
                FLP_UART_TASK_STACK,
                &uart_ingest,
                FLP_UART_TASK_PRIORITY,
                nullptr);

    // Demo button (GPIO ISR + lightweight handler task)
    xTaskCreate(button_task,
                "button_task",
                FLP_BUTTON_TASK_STACK,
                &mesh_manager,
                FLP_BUTTON_TASK_PRIORITY,
                &s_button_task_handle);

    gpio_config_t btn_cfg = {};
    btn_cfg.pin_bit_mask = 1ULL << CONFIG_FLP_DEMO_BUTTON_PIN;
    btn_cfg.mode = GPIO_MODE_INPUT;
    btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_cfg.intr_type = GPIO_INTR_NEGEDGE;
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    // ISR service already installed by LoraTransport::init()
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(isr_ret);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(
        static_cast<gpio_num_t>(CONFIG_FLP_DEMO_BUTTON_PIN),
        button_isr_handler,
        nullptr));

#if CONFIG_FLP_OLED_ENABLED
    xTaskCreate(display_task,
                "display",
                FLP_DISPLAY_TASK_STACK,
                &mesh_manager,
                FLP_DISPLAY_TASK_PRIORITY,
                nullptr);
#endif

    ESP_LOGI(TAG,
             "All tasks created (UART on GPIO %d/%d, button on GPIO %d)",
             CONFIG_FLP_UART_TX_PIN,
             CONFIG_FLP_UART_RX_PIN,
             CONFIG_FLP_DEMO_BUTTON_PIN);
}
