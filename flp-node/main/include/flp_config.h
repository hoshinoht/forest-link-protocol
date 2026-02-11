#pragma once

#define FLP_VERSION "0.1.0"

// Task stack sizes (bytes)
#define FLP_MESH_TASK_STACK     4096
#define FLP_MQTT_TASK_STACK     4096
#define FLP_PROTOCOL_TASK_STACK 4096

// Task priorities
#define FLP_MESH_TASK_PRIORITY     5
#define FLP_MQTT_TASK_PRIORITY     4
#define FLP_PROTOCOL_TASK_PRIORITY 5

// Mesh parameters
#define FLP_MAX_HOPS           8
#define FLP_MAX_NEIGHBORS      16
#define FLP_BROADCAST_RETRIES  3

// Fragment sizes
#define FLP_BLE_MTU            512
#define FLP_LORA_MAX_PAYLOAD   255

// ARQ sliding window
#define FLP_ARQ_WINDOW_SIZE    8
#define FLP_ARQ_TIMEOUT_MS     2000
