#pragma once

#define FLP_VERSION "0.3.3"

/* Task stack sizes (bytes) */
#define FLP_MESH_TASK_STACK 8192
#define FLP_MQTT_TASK_STACK 8192

/* Task priorities (higher = more urgent) */
#define FLP_LORA_RX_TASK_PRIORITY \
    20 /* Timing-critical; must drain FIFO before next RX */
#define FLP_MESH_TASK_PRIORITY \
    15 /* Core routing, must not stall during transfer */
#define FLP_MQTT_TASK_PRIORITY 10 /* Network I/O, tolerates latency */

/* UART ingest API */
#define FLP_UART_TASK_STACK    6144
#define FLP_UART_TASK_PRIORITY 8

/* Demo client (FlpClient) */
#define FLP_CLIENT_TASK_STACK    4096
#define FLP_CLIENT_TASK_PRIORITY 5

/* OLED display — LVGL needs ~6KB+ for lv_timer_handler */
#define FLP_DISPLAY_TASK_STACK    8192
#define FLP_DISPLAY_TASK_PRIORITY 3
#define FLP_DISPLAY_UPDATE_MS     500

/*
 * Maximum ingest file size (10 MB) — pre-allocation guard in uart_ingest
 * checks available PSRAM before allocating, so this only succeeds if memory
 * is actually available.
 */
#define FLP_INGEST_MAX_SIZE (10 * 1024 * 1024)
