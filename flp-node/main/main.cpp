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
#if !CONFIG_FLP_WIFI_DISABLED
#include "mqtt_client.hpp"
#endif

#if CONFIG_FLP_SD_ENABLED
#include "sdcard.hpp"
#endif

#if CONFIG_FLP_OLED_ENABLED
#include "oled_display.hpp"
#endif

static const char *TAG = "flp_main";
static const char *FILENAME = "demo.txt";

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

static flp::MeshManager mesh_manager;
static flp::UartIngest uart_ingest;

#if !CONFIG_FLP_WIFI_DISABLED
static flp::MqttClient mqtt_client;
static EventGroupHandle_t s_wifi_event_group;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
/* Fix 13: event bit set when MQTT first connects, waited on by auto_demo_task */
static constexpr EventBits_t MQTT_CONNECTED_BIT = BIT1;
#endif

#if CONFIG_FLP_OLED_ENABLED
static flp::OledDisplay oled_display;
#endif

/* Demo transfer: callback + size (streaming from SD or in-memory fallback) */
static flp::ReadChunkFn s_demo_read_chunk;
static size_t s_demo_size = 0;

/* Fallback: 8 KB synthetic pattern if SD card is unavailable */
static constexpr size_t FALLBACK_PAYLOAD_SIZE = 8192;
static uint8_t *s_fallback_payload = nullptr;

/* SD card status for deferred logging (early boot logs lost to USB reconnect) */
#if CONFIG_FLP_SD_ENABLED
static const char *s_sd_status = "not attempted";
static esp_err_t s_sd_err = ESP_OK;
static flp::SdReadCache s_sd_cache;
#endif

#if CONFIG_FLP_DEMO_AUTO && !CONFIG_FLP_WIFI_DISABLED
/* Auto demo mode (exit node): waits for MQTT, then periodic transfer */
static void auto_demo_task(void *arg)
{
    auto *mgr = static_cast<flp::MeshManager *>(arg);
    const TickType_t interval =
        pdMS_TO_TICKS(CONFIG_FLP_DEMO_AUTO_INTERVAL_S * 1000);

    /* Fix 13: Wait for MQTT connection via event group instead of polling.
     * s_wifi_event_group / MQTT_CONNECTED_BIT are set by MqttClient on
     * MQTT_EVENT_CONNECTED, so this task blocks without burning CPU. */
    ESP_LOGI(TAG, "Auto demo: waiting for MQTT connection...");
    xEventGroupWaitBits(s_wifi_event_group,
                        MQTT_CONNECTED_BIT,
                        pdFALSE,  /* do not clear the bit */
                        pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "Auto demo: MQTT connected, starting transfers");

    while (true)
    {
        if (!mgr->is_mqtt_connected())
        {
            ESP_LOGW(TAG, "Auto demo: MQTT disconnected, skipping transfer");
            vTaskDelay(interval);
            continue;
        }

        /*
         * Exit nodes with internet serve as relay forwarders — their MQTT
         * pipeline (fragment_publish_queue_ + fragment_ack_queue_) is shared
         * with relay-forwarded fragments.  Running a local-exit transfer
         * blocks relay transfers and pollutes the deferred ACK queue.
         * Skip auto-demo if we have internet; relay nodes will use us as
         * an exit node instead.
         */
        if (mgr->has_internet())
        {
            ESP_LOGI(TAG,
                     "Auto demo: skipping (exit node serves relay transfers)");
            vTaskDelay(interval);
            continue;
        }

        ESP_LOGI(TAG,
                 "Auto demo transfer: demo.txt (%u bytes)",
                 (unsigned) s_demo_size);
        mgr->start_file_transfer("demo.txt", s_demo_size, s_demo_read_chunk);

        /* Wait for transfer to finish before starting the interval timer */
        while (mgr->is_transfer_active())
        {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(TAG, "Auto demo: transfer done, waiting %ds",
                 CONFIG_FLP_DEMO_AUTO_INTERVAL_S);
        vTaskDelay(interval);
    }
}
#elif CONFIG_FLP_DEMO_AUTO && CONFIG_FLP_WIFI_DISABLED
/* Auto demo mode (relay node): waits for gateway discovery, then periodic transfer */
static void auto_demo_task(void *arg)
{
    auto *mgr = static_cast<flp::MeshManager *>(arg);
    const TickType_t interval =
        pdMS_TO_TICKS(CONFIG_FLP_DEMO_AUTO_INTERVAL_S * 1000);

    ESP_LOGI(TAG, "Auto demo (relay): waiting for gateway discovery...");
    while (mgr->get_hops_to_internet() >= 0xFF)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Auto demo (relay): gateway found at %u hops, starting transfers",
             mgr->get_hops_to_internet());

    while (true)
    {
        if (mgr->get_hops_to_internet() >= 0xFF)
        {
            ESP_LOGW(TAG, "Auto demo (relay): no gateway, skipping transfer");
            vTaskDelay(interval);
            continue;
        }
        ESP_LOGI(TAG,
                 "Auto demo transfer: demo.txt (%u bytes)",
                 (unsigned) s_demo_size);
        mgr->start_file_transfer("demo.txt", s_demo_size, s_demo_read_chunk);

        /* Wait for transfer to finish before starting the interval timer */
        while (mgr->is_transfer_active())
        {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        ESP_LOGI(TAG, "Auto demo (relay): transfer done, waiting %ds",
                 CONFIG_FLP_DEMO_AUTO_INTERVAL_S);
        vTaskDelay(interval);
    }
}
#elif !CONFIG_FLP_DEMO_AUTO
/* Manual demo mode: button-triggered transfer */
static TaskHandle_t s_button_task_handle = nullptr;
static TickType_t s_last_button_press = 0;

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

        ESP_LOGI(TAG,
                 "Demo transfer: %s (%u bytes)",
                 FILENAME,
                 (unsigned) s_demo_size);
        mgr->start_file_transfer(FILENAME, s_demo_size, s_demo_read_chunk);
    }
}
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
        /* Task 1: Wire WiFi status into MeshManager */
        mesh_manager.set_has_internet(false);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        /* Task 1: Wire WiFi status into MeshManager */
        mesh_manager.set_has_internet(true);
        /*
         * Fix 7: Do not call update_espnow_broadcast_peer() directly here —
         * this runs in the WiFi event loop task which may hold WiFi driver
         * internals. Schedule the update for the mesh task instead.
         */
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
        status.control_hops_to_internet = mgr->get_control_hops_to_internet();
        status.data_hops_to_internet = mgr->get_data_hops_to_internet();
        status.transfer_active = mgr->is_transfer_active();
        status.filename = mgr->get_transfer_filename();
        status.transfer_pct = mgr->get_transfer_progress();
        status.free_heap_kb = esp_get_free_heap_size() / 1024;
        status.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
        status.cloud_cmd_received = mgr->has_recent_cloud_cmd();

        oled_display.update(status);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FLP_DISPLAY_UPDATE_MS));
    }
}
#endif

extern "C" void app_main()
{
    /*
     * Work around USB-Serial/JTAG stdio/VFS instability on this board by
     * bypassing the default newlib vprintf -> esp_vfs_write() path for logs.
     */
    esp_log_set_vprintf(rom_safe_log_vprintf);

    ESP_LOGI(TAG, "FLP Node v%s starting...", FLP_VERSION);

    /* PSRAM / heap diagnostics */
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

    /*
     * Init OLED before WiFi — WiFi PHY calibration on ESP32-S3 rev v0.2
     * can corrupt PSRAM heap metadata (shared MSPI bus).  If
     * i2c_new_master_bus runs after that, heap_caps_calloc iterates a
     * corrupted PSRAM region and crashes (EXCVADDR = 0xAAAAAAAA).
     * Allocating everything the display needs *before* WiFi start avoids
     * the issue entirely.
     */
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

    /* Set fixed channel for relay nodes (must match exit node AP channel) */
    ESP_ERROR_CHECK(
        esp_wifi_set_channel(CONFIG_FLP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG,
             "WiFi STA started (no AP) for ESP-NOW, ch=%d",
             CONFIG_FLP_ESPNOW_CHANNEL);

    /*
     * ESP32-S3 rev v0.2 workaround: WiFi PHY calibration writes to NVS
     * flash via the shared MSPI bus, which can corrupt PSRAM heap metadata.
     * After calibration data is cached in NVS (second boot onward) the
     * write doesn't happen and there is no corruption.  Detect this and
     * auto-reboot so the second boot succeeds cleanly.
     *
     * NOTE: heap_caps_check_integrity() itself crashes when the heap
     * metadata lock is corrupted (EXCVADDR 0xAAAAAAAA).  Use a simple
     * canary write/read instead — it is safe regardless of heap state.
     */
#if CONFIG_SPIRAM
    {
        volatile uint32_t *canary =
            (volatile uint32_t *)heap_caps_malloc(16, MALLOC_CAP_SPIRAM);
        bool psram_ok = false;
        if (canary) {
            canary[0] = 0xDEADBEEF;
            canary[1] = 0xCAFEBABE;
            canary[2] = 0x12345678;
            canary[3] = 0x9ABCDEF0;
            psram_ok = (canary[0] == 0xDEADBEEF && canary[1] == 0xCAFEBABE &&
                        canary[2] == 0x12345678 && canary[3] == 0x9ABCDEF0);
            heap_caps_free((void *)canary);
        }
        if (!psram_ok) {
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

    /* Same PSRAM canary check for the gateway path (see relay path above) */
#if CONFIG_SPIRAM
    {
        volatile uint32_t *canary =
            (volatile uint32_t *)heap_caps_malloc(16, MALLOC_CAP_SPIRAM);
        bool psram_ok = false;
        if (canary) {
            canary[0] = 0xDEADBEEF;
            canary[1] = 0xCAFEBABE;
            canary[2] = 0x12345678;
            canary[3] = 0x9ABCDEF0;
            psram_ok = (canary[0] == 0xDEADBEEF && canary[1] == 0xCAFEBABE &&
                        canary[2] == 0x12345678 && canary[3] == 0x9ABCDEF0);
            heap_caps_free((void *)canary);
        }
        if (!psram_ok) {
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

    /*
     * ESP-NOW init is handled by EspNowTransport::init() called from
     * MeshManager
     */

    /* SD card init — after WiFi to avoid VFS/SPI conflicts */
#if CONFIG_FLP_SD_ENABLED
    s_sd_err = flp::sdcard_init();
    if (s_sd_err == ESP_OK)
    {
        char sd_path[96] = {};
        std::snprintf(sd_path, sizeof(sd_path), "/sdcard/%s", FILENAME);
        esp_err_t cache_err = s_sd_cache.open(sd_path);
        if (cache_err == ESP_OK)
        {
            s_demo_size = s_sd_cache.file_size();
            s_demo_read_chunk = [](uint8_t *buf,
                                   size_t offset,
                                   size_t len) -> size_t
            {
                return s_sd_cache.read(buf, offset, len);
            };
            s_sd_status = "OK";
            ESP_LOGI(TAG,
                     "Loaded %s from SD card: %u bytes "
                     "(%zu KB cache in PSRAM%s)",
                     FILENAME,
                     (unsigned) s_demo_size,
                     s_sd_cache.cache_size() / 1024,
                     s_sd_cache.fully_cached()
                         ? ", fully resident — zero SD I/O during transfer"
                         : "");
        }
        else
        {
            s_sd_status = "file not found";
            ESP_LOGW(TAG, "%s not found on SD card, using fallback", FILENAME);
        }
    }
    else
    {
        s_sd_status = "mount failed";
        ESP_LOGW(TAG, "SD card init failed, using fallback payload");
    }
#endif
    if (!s_demo_read_chunk)
    {
        s_fallback_payload = static_cast<uint8_t *>(
            heap_caps_malloc(FALLBACK_PAYLOAD_SIZE, MALLOC_CAP_SPIRAM));
        if (!s_fallback_payload)
        {
            ESP_LOGE(TAG, "Failed to allocate fallback payload in PSRAM");
        }
        else
        {
            for (size_t i = 0; i < FALLBACK_PAYLOAD_SIZE; i++)
            {
                s_fallback_payload[i] = static_cast<uint8_t>('A' + (i % 26));
            }
            s_demo_size = FALLBACK_PAYLOAD_SIZE;
            s_demo_read_chunk = flp::TransferEngine::make_buffer_reader(
                s_fallback_payload, FALLBACK_PAYLOAD_SIZE);
        }
    }

    mesh_manager.set_lora_rx_priority(FLP_LORA_RX_TASK_PRIORITY);
    mesh_manager.init();
    mesh_manager.subscribe_topic("config");
    mesh_manager.subscribe_topic("alert");

#if !CONFIG_FLP_WIFI_DISABLED
    /* Task 6: Wire MQTT client to mesh manager for file upload bridge */
    mqtt_client.set_node_addr(mesh_manager.get_addr());
    /* Fix 13: Register event group so auto_demo_task can wait for MQTT
     * instead of polling. Must be called before mqtt_client.init(). */
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

#if CONFIG_FLP_DEMO_AUTO
    /* Auto demo mode: periodic transfer task (both exit and relay) */
    xTaskCreate(auto_demo_task,
                "auto_demo",
                FLP_BUTTON_TASK_STACK,
                &mesh_manager,
                FLP_BUTTON_TASK_PRIORITY,
                nullptr);
    ESP_LOGI(TAG,
             "Auto demo enabled: transfer every %d seconds",
             CONFIG_FLP_DEMO_AUTO_INTERVAL_S);
#elif !CONFIG_FLP_DEMO_AUTO
    /* Demo button (GPIO ISR + lightweight handler task) */
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

    /* ISR service already installed by LoraTransport::init() */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(isr_ret);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(
        static_cast<gpio_num_t>(CONFIG_FLP_DEMO_BUTTON_PIN),
        button_isr_handler,
        nullptr));
#endif

#if CONFIG_FLP_OLED_ENABLED
    {
        /*
         * display_task is low priority (3) and non-critical.  Allocate its
         * stack from PSRAM so it doesn't compete with WiFi / mesh / ARQ
         * for scarce internal SRAM.  Log if creation fails.
         */
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

#if CONFIG_FLP_DEMO_AUTO
    ESP_LOGI(TAG,
             "All tasks created (UART on GPIO %d/%d, auto demo every %ds)",
             CONFIG_FLP_UART_TX_PIN,
             CONFIG_FLP_UART_RX_PIN,
             CONFIG_FLP_DEMO_AUTO_INTERVAL_S);
#elif !CONFIG_FLP_DEMO_AUTO
    ESP_LOGI(TAG,
             "All tasks created (UART on GPIO %d/%d, button on GPIO %d)",
             CONFIG_FLP_UART_TX_PIN,
             CONFIG_FLP_UART_RX_PIN,
             CONFIG_FLP_DEMO_BUTTON_PIN);
#else
    ESP_LOGI(TAG,
             "All tasks created (UART on GPIO %d/%d, relay mode)",
             CONFIG_FLP_UART_TX_PIN,
             CONFIG_FLP_UART_RX_PIN);
#endif

    /* Log demo payload source (visible even when monitor reconnects late) */
#if CONFIG_FLP_SD_ENABLED
    ESP_LOGI(TAG,
             "Demo payload: %u bytes (%s) [SD: %s err=%s(0x%x) CS=%d MOSI=%d MISO=%d SCK=%d]",
             (unsigned) s_demo_size,
             s_demo_size == FALLBACK_PAYLOAD_SIZE ? "fallback" : "SD card",
             s_sd_status,
             esp_err_to_name(s_sd_err),
             s_sd_err,
             CONFIG_FLP_SD_CS,
             CONFIG_FLP_SD_MOSI,
             CONFIG_FLP_SD_MISO,
             CONFIG_FLP_SD_SCK);
#else
    ESP_LOGI(TAG,
             "Demo payload: %u bytes (fallback, SD disabled)",
             (unsigned) s_demo_size);
#endif
}
