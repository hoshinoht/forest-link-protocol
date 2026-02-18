#pragma once

#define FLP_VERSION "0.1.0"

// Task stack sizes (bytes)
#define FLP_MESH_TASK_STACK     8192
#define FLP_MQTT_TASK_STACK     6144
#define FLP_PROTOCOL_TASK_STACK 4096

// Task priorities (higher = more urgent)
#define FLP_LORA_RX_TASK_PRIORITY \
    20 // Timing-critical; must drain FIFO before next RX
#define FLP_MESH_TASK_PRIORITY \
    15 // Core routing, must not stall during transfer
#define FLP_MQTT_TASK_PRIORITY     10 // Network I/O, tolerates latency
#define FLP_PROTOCOL_TASK_PRIORITY 5  // Lightweight tick, lowest

// Mesh parameters
#define FLP_MAX_HOPS          8
#define FLP_MAX_NEIGHBORS     16
#define FLP_BROADCAST_RETRIES 3

// Fragment sizes
#define FLP_BLE_MTU          512
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

// Maximum ingest file size (3 MB)
#define FLP_INGEST_MAX_SIZE (3 * 1024 * 1024)
