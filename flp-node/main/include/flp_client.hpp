#pragma once

/*
 * FlpClient — In-process demo client that exercises the UART ingest API.
 *
 * Replaces the old auto_demo_task / button_task scattered in main.cpp.
 * Loads demo payload from SD card (or generates a fallback).
 * SD-backed payloads are streamed directly into the transfer engine to
 * avoid duplicating large buffers in PSRAM; fallback payloads still use
 * the UartIngest::file_begin/data/end in-process path.
 */

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#if CONFIG_FLP_SD_ENABLED
#include "sdcard.hpp"
#endif

namespace flp
{

class UartIngest;
class MeshManager;

class FlpClient
{
  public:
    FlpClient() = default;

    /* Call after uart_ingest.init() and mesh_manager.init().
     * wifi_events may be nullptr for relay nodes. */
    void init(UartIngest *api, MeshManager *mgr,
              EventGroupHandle_t wifi_events);

    /* FreeRTOS task entry — runs forever */
    void run();

  private:
    void load_demo_payload();
    void do_transfer();
    void wait_for_gateway();
    void wait_for_mqtt();

    UartIngest *api_ = nullptr;
    MeshManager *mgr_ = nullptr;
    EventGroupHandle_t wifi_events_ = nullptr;

    /* Demo payload */
    uint8_t *payload_buf_ = nullptr;
    size_t payload_size_ = 0;
    char filename_[64] = {};
    bool use_sd_stream_ = false;

#if CONFIG_FLP_SD_ENABLED
    SdReadCache sd_cache_;
#endif
};

} /* namespace flp */
