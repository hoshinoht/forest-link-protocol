#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "flp_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mesh_manager.hpp"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "nvs_flash.h"
#include "uart_ingest.hpp"
#include "flp_client.hpp"
#if !CONFIG_FLP_WIFI_DISABLED
#include "mqtt_client.hpp"
#endif

#if CONFIG_FLP_OLED_ENABLED
#include "oled_display.hpp"
#endif

static const char *TAG = "flp_main";

static int rom_safe_log_vprintf(const char *fmt, va_list args)
{
    char buf[384];
    int ret = vsnprintf(buf, sizeof(buf), fmt, args);
    if (ret > 0)
    {
        esp_rom_printf("%s", buf);
    }
    return ret;
}

static bool verify_psram_integrity()
{
    volatile uint32_t *canary =
        (volatile uint32_t *)heap_caps_malloc(16, MALLOC_CAP_SPIRAM);
    bool psram_ok = false;
    if (canary)
    {
        canary[0] = 0xDEADBEEF;
        canary[1] = 0xCAFEBABE;
        canary[2] = 0x12345678;
        canary[3] = 0x9ABCDEF0;
        psram_ok = (canary[0] == 0xDEADBEEF && canary[1] == 0xCAFEBABE &&
                    canary[2] == 0x12345678 && canary[3] == 0x9ABCDEF0);
        heap_caps_free((void *)canary);
    }
    return psram_ok;
}

static flp::MeshManager mesh_manager;
static flp::UartIngest uart_ingest;
static flp::FlpClient flp_client;

#if !CONFIG_FLP_WIFI_DISABLED
static flp::MqttClient mqtt_client;
static EventGroupHandle_t s_wifi_event_group;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
static constexpr EventBits_t MQTT_CONNECTED_BIT = BIT1;
#endif

#if CONFIG_FLP_OLED_ENABLED
static flp::OledDisplay oled_display;
#endif

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
        mesh_manager.set_has_internet(false);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        mesh_manager.set_has_internet(true);
        mesh_manager.request_espnow_peer_update();
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

static void flp_client_task(void *arg)
{
    ESP_LOGI(TAG, "flp_client_task started");
    auto *client = static_cast<flp::FlpClient *>(arg);
    client->run();
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
        mgr->snapshot_display_state(status);

        oled_display.update(status);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FLP_DISPLAY_UPDATE_MS));
    }
}
#endif

extern "C" void app_main()
{
    esp_log_set_vprintf(rom_safe_log_vprintf);

    ESP_LOGI(TAG, "FLP Node v%s starting...", FLP_VERSION);

#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM size: %d bytes, free: %d bytes",
             esp_psram_get_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    ESP_LOGI(TAG, "Internal free heap: %zu bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Total free heap: %zu bytes",
             esp_get_free_heap_size());

    /* Initialize NVS (required for WiFi + ESP-NOW) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Init OLED before WiFi (PSRAM corruption workaround) */
#if CONFIG_FLP_OLED_ENABLED
    oled_display.init(
        CONFIG_FLP_OLED_SDA, CONFIG_FLP_OLED_SCL, CONFIG_FLP_OLED_RST);
#endif

#if CONFIG_FLP_WIFI_DISABLED
    /* Relay-only node: WiFi started in STA mode (no AP connect) for ESP-NOW */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t relay_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&relay_cfg));
    esp_wifi_internal_set_log_level(WIFI_LOG_ERROR);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(
        esp_wifi_set_channel(CONFIG_FLP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG,
             "WiFi STA started (no AP) for ESP-NOW, ch=%d",
             CONFIG_FLP_ESPNOW_CHANNEL);

#if CONFIG_SPIRAM
    {
        if (!verify_psram_integrity()) {
            ESP_LOGE(TAG,
                     "PSRAM corrupted after WiFi PHY calibration "
                     "(ESP32-S3 rev v0.2 MSPI bus issue). "
                     "Rebooting — next boot will use cached cal data.");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
#endif

#else
    /* Initialize WiFi station */
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_internal_set_log_level(WIFI_LOG_ERROR);

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

#if CONFIG_SPIRAM
    {
        if (!verify_psram_integrity()) {
            ESP_LOGE(TAG,
                     "PSRAM corrupted after WiFi PHY calibration "
                     "(ESP32-S3 rev v0.2 MSPI bus issue). "
                     "Rebooting — next boot will use cached cal data.");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
#endif
#endif

    /* ── Initialize components ────────────────────────────────────────── */

    mesh_manager.set_lora_rx_priority(FLP_LORA_RX_TASK_PRIORITY);
    mesh_manager.init();
    mesh_manager.subscribe_topic("config");
    mesh_manager.subscribe_topic("alert");

#if CONFIG_FLP_LED_GPIO >= 0
    {
        gpio_config_t led_cfg = {};
        led_cfg.pin_bit_mask = 1ULL << CONFIG_FLP_LED_GPIO;
        led_cfg.mode = GPIO_MODE_OUTPUT;
        led_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        led_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        led_cfg.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&led_cfg));
        gpio_set_level(static_cast<gpio_num_t>(CONFIG_FLP_LED_GPIO), 0);
        ESP_LOGI(TAG, "LED output configured on GPIO %d", CONFIG_FLP_LED_GPIO);
    }
#endif

#if !CONFIG_FLP_WIFI_DISABLED
    mqtt_client.set_node_addr(mesh_manager.get_addr());
    mqtt_client.set_connected_event_group(s_wifi_event_group,
                                          MQTT_CONNECTED_BIT);
    mqtt_client.init();
    mesh_manager.set_mqtt_client(&mqtt_client);
#endif

    /* UART ingest API */
    uart_ingest.init(UART_NUM_2,
                     CONFIG_FLP_UART_TX_PIN,
                     CONFIG_FLP_UART_RX_PIN,
                     &mesh_manager);
    mesh_manager.set_uart_ingest(&uart_ingest);

    /* FlpClient: demo driver that exercises the UART API in-process */
    flp_client.init(&uart_ingest, &mesh_manager,
#if !CONFIG_FLP_WIFI_DISABLED
                    s_wifi_event_group
#else
                    nullptr
#endif
    );

    /* ── Launch tasks ─────────────────────────────────────────────────── */

    /* Pin mesh_task to CPU1 so it never competes with WiFi/ESP-NOW
     * callbacks on CPU0.  Faster TX slot turnover + faster ARQ ticks. */
    xTaskCreatePinnedToCore(mesh_task,
                            "mesh_task",
                            FLP_MESH_TASK_STACK,
                            &mesh_manager,
                            FLP_MESH_TASK_PRIORITY,
                            nullptr,
                            1);
#if !CONFIG_FLP_WIFI_DISABLED
    /* Pin mqtt_task to CPU0 — lwIP/TCP stack already runs on CPU0, so
     * keeping MQTT I/O on the same core avoids cross-core IPC on every
     * TCP call.  mesh_task gets uncontested CPU1 for ARQ ticks. */
    xTaskCreatePinnedToCore(mqtt_task,
                            "mqtt_task",
                            FLP_MQTT_TASK_STACK,
                            &mqtt_client,
                            FLP_MQTT_TASK_PRIORITY,
                            nullptr,
                            0);
#endif
    xTaskCreate(uart_ingest_task,
                "uart_ingest",
                FLP_UART_TASK_STACK,
                &uart_ingest,
                FLP_UART_TASK_PRIORITY,
                nullptr);

#if CONFIG_FLP_DEMO_AUTO
    xTaskCreate(flp_client_task,
                "flp_client",
                FLP_CLIENT_TASK_STACK,
                &flp_client,
                FLP_CLIENT_TASK_PRIORITY,
                nullptr);
    ESP_LOGI(TAG,
             "Auto demo enabled: transfer every %d seconds",
             CONFIG_FLP_DEMO_AUTO_INTERVAL_S);
#endif

#if CONFIG_FLP_OLED_ENABLED
    {
        StaticTask_t *tcb = (StaticTask_t *)heap_caps_calloc(
            1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        StackType_t *stack = (StackType_t *)heap_caps_calloc(
            FLP_DISPLAY_TASK_STACK, sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tcb && stack) {
            xTaskCreateStatic(display_task,
                              "display",
                              FLP_DISPLAY_TASK_STACK,
                              &mesh_manager,
                              FLP_DISPLAY_TASK_PRIORITY,
                              stack,
                              tcb);
        } else {
            ESP_LOGE(TAG, "Failed to allocate display_task (tcb=%p stack=%p)",
                     tcb, stack);
        }
    }
#endif

    ESP_LOGI(TAG,
             "All tasks created (UART on GPIO %d/%d)",
             CONFIG_FLP_UART_TX_PIN,
             CONFIG_FLP_UART_RX_PIN);
}
