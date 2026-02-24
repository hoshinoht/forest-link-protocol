#pragma once

#define FLP_VERSION "0.1.0"

// Task stack sizes (bytes)
#define FLP_MESH_TASK_STACK 8192
#define FLP_MQTT_TASK_STACK 6144

// Task priorities (higher = more urgent)
#define FLP_LORA_RX_TASK_PRIORITY \
    20 // Timing-critical; must drain FIFO before next RX
#define FLP_MESH_TASK_PRIORITY \
    15 // Core routing, must not stall during transfer
#define FLP_MQTT_TASK_PRIORITY 10 // Network I/O, tolerates latency

// Mesh parameters
#define FLP_MAX_HOPS          8
#define FLP_MAX_NEIGHBORS     16
#define FLP_BROADCAST_RETRIES 3

// Fragment sizes
#define FLP_ESPNOW_MTU       250
#define FLP_LORA_MAX_PAYLOAD 255

// ARQ sliding window
#define FLP_ARQ_WINDOW_SIZE 8
#define FLP_ARQ_TIMEOUT_MS  2000

// UART ingest API
#define FLP_UART_TASK_STACK    6144
#define FLP_UART_TASK_PRIORITY 8

// Demo button
#define FLP_BUTTON_TASK_STACK    2048
#define FLP_BUTTON_TASK_PRIORITY 5
#define FLP_BUTTON_DEBOUNCE_MS   2000

// OLED display
#define FLP_DISPLAY_TASK_STACK    3072
#define FLP_DISPLAY_TASK_PRIORITY 3
#define FLP_DISPLAY_UPDATE_MS     500

// Maximum ingest file size (10 MB) — pre-allocation guard in uart_ingest
// checks available PSRAM before allocating, so this only succeeds if memory
// is actually available.
#define FLP_INGEST_MAX_SIZE (10 * 1024 * 1024)
