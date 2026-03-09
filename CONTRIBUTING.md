# Contributing to Forest Link Protocol

ESP32-S3 mesh networking firmware using ESP-NOW + LoRa transports, ESP-IDF v5.5, FreeRTOS, C++17.

---

## Table of Contents

1. [FreeRTOS Task Design](#1-freertos-task-design)
2. [Queues and Semaphores](#2-queues-and-semaphores)
3. [Memory Management](#3-memory-management)
4. [ESP-NOW Rules](#4-esp-now-rules)
5. [Timer and Time Sources](#5-timer-and-time-sources)
6. [WiFi Event Handling](#6-wifi-event-handling)
7. [LoRa / SPI](#7-lora--spi)
8. [Thread Safety and Atomics](#8-thread-safety-and-atomics)
9. [Sequence Number Arithmetic](#9-sequence-number-arithmetic)
10. [Input Validation](#10-input-validation)
11. [Testing Requirements](#11-testing-requirements)
12. [Code Style](#12-code-style)
13. [Commit Messages](#13-commit-messages)
14. [Quick Reference](#14-quick-reference)

---

## 1. FreeRTOS Task Design

### Stack sizing

| Task type | Minimum stack |
|---|---|
| Simple logic, no C++ objects | 2 KB |
| C++ objects, moderate call depth | 4 KB |
| JSON / string formatting, MQTT | 8 KB |
| LoRa RX / mesh manager | 8 KB |

Always measure with `uxTaskGetStackHighWaterMark(NULL)` during development and leave at least 512 bytes of headroom. Enable `configCHECK_FOR_STACK_OVERFLOW=2` in `sdkconfig.defaults` for debug builds.

### Task priorities

Higher number = higher priority on ESP-IDF.

```
ISR-deferred tasks (radio RX, ESP-NOW RX)   >  processing tasks (mesh)  >  background (MQTT, demo)
```

Never invert this ordering — a low-priority task holding a resource that a high-priority task waits on causes priority inversion.

### Task lifecycle

Tasks must **never return** from their entry function. End every task with:

```cpp
vTaskDelete(NULL);  // must be the last line
```

### Periodic tasks

Use `vTaskDelayUntil` instead of `vTaskDelay` so drift does not accumulate over time:

```cpp
TickType_t last_wake = xTaskGetTickCount();
for (;;) {
    do_work();
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
}
```

### Blocking calls

**Never use `portMAX_DELAY`** except for explicitly documented sentinel tasks (e.g. a task whose sole job is to wait forever for one event). For all other blocking calls use a bounded timeout and handle the timeout case:

```cpp
// Bad
xSemaphoreTake(spi_mutex_, portMAX_DELAY);

// Good
if (xSemaphoreTake(spi_mutex_, pdMS_TO_TICKS(200)) != pdTRUE) {
    ESP_LOGE(TAG, "spi_mutex_ timeout — radio may be locked up");
    return ESP_ERR_TIMEOUT;
}
```

---

## 2. Queues and Semaphores

### ISR / WiFi callback context

Any function called from a hardware ISR or from the WiFi driver task (ESP-NOW callbacks, WiFi event callbacks) must use the `FromISR` variants:

```cpp
// From ISR or WiFi callback
BaseType_t higher_prio_woken = pdFALSE;
xQueueSendFromISR(queue_, &item, &higher_prio_woken);
portYIELD_FROM_ISR(higher_prio_woken);

// From normal task
xQueueSend(queue_, &item, pdMS_TO_TICKS(0));
```

### Mutex vs binary semaphore

| Use case | Type |
|---|---|
| Mutual exclusion (protect shared data) | `xSemaphoreCreateMutex()` |
| Signalling between tasks | `xSemaphoreCreateBinary()` |
| ISR → task notification | `xTaskNotifyFromISR()` |

**Never take a mutex from an ISR.** Mutexes use priority inheritance, which is not valid in ISR context and will assert.

### Multi-step atomic sequences

A single mutex protecting individual transactions is **not enough** when a sequence of transactions must be atomic. Use a separate operation-level mutex:

```cpp
// Wrong: RX task can interleave between these two SPI calls
spi_write(CMD_STANDBY);
spi_write(CMD_SET_TX);      // RX task modifies radio state here

// Correct: hold radio_op_mutex_ for the full sequence
xSemaphoreTake(radio_op_mutex_, pdMS_TO_TICKS(200));
spi_write(CMD_STANDBY);
spi_write(CMD_SET_TX);
xSemaphoreGive(radio_op_mutex_);
```

### Queue depth

Size queues for burst traffic. A depth of 4 is rarely sufficient for radio RX under mesh traffic. Use at least 8–16 for packet queues, and always send with `timeout=0` from callbacks (drop rather than block).

---

## 3. Memory Management

### PSRAM allocation

Always pair `heap_caps_malloc` with `heap_caps_free`. Never mix with `free()`:

```cpp
// Allocate
buf_ = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));

// Release — NOT free(buf_)
heap_caps_free(buf_);
buf_ = nullptr;
```

Mismatched alloc/free corrupts heap metadata on ESP-IDF 5.x.

### Stack allocation limits

Never put large buffers on task stacks. `uint8_t buf[MAX_MTU]` (250 bytes) on a stack that is called from multiple frames will overflow a 2–4 KB stack.

```cpp
// Bad — 250 bytes on stack inside a call chain
void process() {
    uint8_t buf[MAX_MTU];
    ...
}

// Good — static or class member
class Processor {
    uint8_t buf_[MAX_MTU];  // lives in BSS, not on stack
};
```

### Check before large allocation

```cpp
size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
if (avail < required_size) {
    ESP_LOGE(TAG, "insufficient PSRAM: need %u have %u", required_size, avail);
    return ESP_ERR_NO_MEM;
}
```

### ISR / callback context

Never call `malloc`, `free`, `new`, or `delete` from ISR context or from ESP-NOW / WiFi callbacks.

### Buffer pool

Use the project's `BufferPool` for packet-sized allocations instead of ad-hoc `malloc`. The pool is lock-free and safe from task context:

```cpp
BufferSlab *slab = pool_.acquire();   // returns nullptr on exhaustion
if (!slab) { /* drop packet */ return; }
pool_.add_ref(slab);    // if handing to another owner
pool_.release(slab);    // when done
```

---

## 4. ESP-NOW Rules

### Receive callback (`on_recv`)

The `on_recv` callback runs inside the **WiFi driver task**, which holds ESP-NOW internal locks. Violating any of these rules causes a deadlock or assert:

| Rule | Reason |
|---|---|
| **Never call `esp_now_add_peer()` or `esp_now_del_peer()`** | Tries to re-acquire the same lock → deadlock |
| **Copy data immediately** | The `data` pointer is invalid after the callback returns |
| **Never block** | Will stall the entire WiFi driver |
| **Use `xQueueSendFromISR` with timeout=0** | Drop packets rather than block |

**Correct pattern — register peers from a task:**

```cpp
// In on_recv: enqueue MAC for deferred registration
PendingPeer pending;
memcpy(pending.mac, mac_addr, 6);
xQueueSendFromISR(pending_peer_queue_, &pending, &woken);
portYIELD_FROM_ISR(woken);

// In mesh task loop: drain and register safely
void drain_pending_peers() {
    PendingPeer p;
    while (xQueueReceive(pending_peer_queue_, &p, 0) == pdTRUE) {
        add_peer_if_new(p.mac);  // esp_now_add_peer() safe here
    }
}
```

### Send callback

Never call `esp_now_send()` from within the ESP-NOW send callback. The send callback also runs in the WiFi task.

### Channel changes

Only call `esp_wifi_set_channel()` from a task context (not from callbacks), only when the channel actually changes, and only when no transmission is in progress:

```cpp
if (new_channel != current_channel_) {
    esp_wifi_set_channel(new_channel, WIFI_SECOND_CHAN_NONE);
    current_channel_ = new_channel;
}
```

---

## 5. Timer and Time Sources

### `esp_timer_get_time()`

Returns microseconds as `int64_t`. **Never cast to `uint32_t` before performing subtraction** — you will silently truncate and get wrong deltas after ~71 minutes.

```cpp
// Bad
uint32_t t0 = (uint32_t)esp_timer_get_time();
uint32_t elapsed = (uint32_t)esp_timer_get_time() - t0;  // wraps at 71 min

// Good
int64_t t0_us = esp_timer_get_time();
int64_t elapsed_ms = (esp_timer_get_time() - t0_us) / 1000;
```

For millisecond timestamps stored as `uint32_t` (e.g. `last_seen_ms`), cast only after dividing:

```cpp
uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
```

This wraps every ~49.7 days. Delta arithmetic is still correct as long as both operands are cast consistently.

### `esp_timer` callbacks

`esp_timer` callbacks run in a dedicated high-priority task. They must:
- Never block
- Never call `vTaskDelay`
- Never take a mutex that a lower-priority task may hold

---

## 6. WiFi Event Handling

Event loop callbacks run on the default event loop task. Keep them fast:

```cpp
// Bad — peer registration from event loop
static void wifi_event_handler(void *arg, ...) {
    esp_now_del_peer(old_mac);   // may contend with WiFi driver internals
    esp_now_add_peer(&peer_cfg);
}

// Good — set a flag, handle in mesh task
static void wifi_event_handler(void *arg, ...) {
    mesh_manager.request_espnow_peer_update();  // atomic flag only
}
```

Never call `esp_now_add_peer()`, `esp_now_del_peer()`, or `esp_wifi_set_channel()` from event loop callbacks.

---

## 7. LoRa / SPI

### Two-level locking

| Lock | Scope | Purpose |
|---|---|---|
| `spi_mutex_` | Single SPI transaction | Prevent byte-level corruption |
| `radio_op_mutex_` | Full multi-step sequence | Prevent state interleaving between TX and RX tasks |

Always acquire `radio_op_mutex_` for multi-step sequences (standby → configure → TX/RX). Release it only after the sequence is complete. The SPI mutex is taken/given within each individual `write_command` / `read_command` call.

### ISR pin handler

```cpp
// ISR must be in IRAM if ESP_INTR_FLAG_IRAM is used
void IRAM_ATTR dio1_isr_handler(void *arg) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(rx_task_handle_, &woken);
    portYIELD_FROM_ISR(woken);
}
```

Never do SPI or GPIO work directly in the ISR — notify the RX task and return.

### Bounded waits

```cpp
// Waiting for hardware to complete — use a meaningful timeout
if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
    ESP_LOGW(TAG, "radio TX timeout — possible hardware fault");
    // attempt recovery
}
```

---

## 8. Thread Safety and Atomics

### Use `std::atomic` for shared flags

```cpp
// Bad — compiler may cache in register, other task never sees update
bool connected_ = false;

// Good
std::atomic<bool> connected_{false};
```

`volatile` is **not** a substitute for atomics on a dual-core processor. It prevents register caching but does not provide memory ordering guarantees across cores.

### Logging from high-priority contexts

`ESP_LOGI` / `ESP_LOGW` / `ESP_LOGE` acquire an internal mutex. Calling them from the ESP-NOW receive callback or from a high-priority radio task causes priority inversion.

**Pattern — count in ISR-safe context, log from task:**

```cpp
// In high-priority / callback context
pool_exhaustion_count_.fetch_add(1, std::memory_order_relaxed);

// In mesh task (low-priority, every N seconds)
uint32_t drops = pool_exhaustion_count_.exchange(0);
if (drops > 0) {
    ESP_LOGW(TAG, "BufferPool exhausted %lu times in last interval", drops);
}
```

---

## 9. Sequence Number Arithmetic

`uint16_t` sequence numbers wrap at 65535 → 0. Plain `>` comparison breaks after wrap — a freshly wrapped seq=5 compares as *less than* the old seq=65000.

Use RFC 1982 serial number arithmetic for all sequence comparisons:

```cpp
/// Returns true if sequence number 'a' is newer than 'b'.
/// Handles uint16_t wrap-around correctly.
static inline bool seq_newer(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}
```

Apply this anywhere DSDV `inet_seq` or ARQ sequence numbers are compared:

```cpp
// Bad
if (seq > neighbor.inet_seq) { ... }

// Good
if (seq_newer(seq, neighbor.inet_seq)) { ... }
```

---

## 10. Input Validation

Validate all lengths and indices at the boundary where untrusted data enters (radio RX, UART ingest). Do not assume the remote peer sends well-formed data.

### Buffer length

```cpp
bool ingest(uint16_t seq, const uint8_t *data, size_t len, bool is_parity)
{
    if (len > MAX_MTU) return false;  // validate before any memcpy
    memcpy(slots_[idx].data, data, len);
    ...
}
```

### Array index before bitmap access

```cpp
uint16_t byte_idx = seq / 8;
uint16_t bitmap_bytes = (total_fragments_ + 7) / 8;
if (byte_idx >= bitmap_bytes) {
    ESP_LOGE(TAG, "seq %u out of bitmap range", seq);
    return false;
}
recv_bitmap_[byte_idx] |= (1 << (seq % 8));
```

### Unsigned subtraction underflow

```cpp
// Bad — if offset > file_size_, copy_len wraps to huge value
size_t copy_len = file_size_ - offset;

// Good
if (data_idx >= total_fragments_) {
    ESP_LOGE(TAG, "invalid data_idx %u", data_idx);
    return false;
}
size_t offset = data_idx * fragment_size_;
size_t copy_len = file_size_ - offset;
```

---

## 11. Testing Requirements

### What needs tests

| Component | Testable without hardware? | Required? |
|---|---|---|
| `PacketHeader` bitfield encoding | Yes | Yes |
| `FecEncoder` / `FecDecoder` | Yes | Yes |
| `RouteTable` (mock `esp_timer`) | Yes | Yes |
| `BufferPool` | Yes | Yes |
| `SelectiveRepeat` (pure logic) | Yes | Yes |
| ESP-NOW transport | No (WiFi driver) | Integration only |
| LoRa transport | No (SPI hardware) | Integration only |

### Running tests

```bash
cd flp-node
idf.py -C test -DIDF_TARGET=esp32s3 build
# Flash and monitor to see Unity output
idf.py -C test -p /dev/ttyUSB0 flash monitor
```

### Writing tests

- Place test files in `flp-node/test/main/test_<component>.cpp`
- Add a `void run_<component>_tests(void)` runner and call it from `test_main.cpp`
- Do **not** define `setUp`/`tearDown` in individual test files — the single pair in `test_main.cpp` calls all fixture resets
- Mock ESP-IDF headers by placing stub headers in `test/mocks/` — they shadow system headers via include path ordering

```cpp
// test/mocks/esp_timer.h — controllable time source
extern int64_t g_mock_time_us;
static inline int64_t esp_timer_get_time(void) { return g_mock_time_us; }
```

### Test structure

```
flp-node/test/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── test_main.cpp          # UNITY_BEGIN/END, single setUp/tearDown
│   ├── test_packet.cpp
│   ├── test_fec.cpp
│   ├── test_route_table.cpp
│   ├── test_buffer_pool.cpp
│   └── test_selective_repeat.cpp
└── mocks/
    ├── esp_timer.h
    ├── esp_log.h
    ├── esp_heap_caps.h
    └── mock_impl.cpp
```

---

## 12. Code Style

- **Formatter:** clang-format (config in repo root). Run before every commit.
- **Log tags:** unique per component, file-scoped: `static const char *TAG = "mesh_mgr";`
- **Hardware constants:** name after the actual chip: `SX1280_SPI_CLOCK_HZ`, not `SX1276_*`
- **Includes:** system headers first, then ESP-IDF, then project headers
- **Namespaces:** all project code lives in `namespace flp`
- **Error handling:** always check ESP-IDF return values; propagate `esp_err_t` up the call chain

---

## 13. Commit Messages

```
type(scope): short summary, 72 chars max

- What changed and why (not just what — git diff shows the what)
- Reference issues: Fixes #42
```

| Type | When to use |
|---|---|
| `fix` | Bug fix |
| `feat` | New feature |
| `refactor` | Code restructure, no behaviour change |
| `test` | Adding or updating tests |
| `docs` | Documentation only |
| `chore` | Build system, dependencies, tooling |

---

## 14. Quick Reference

Common violations and their fixes:

| Violation | Fix |
|---|---|
| `esp_now_add_peer()` in `on_recv` | Enqueue MAC; register from mesh task |
| `esp_wifi_set_channel()` without channel-diff guard | Check `new_ch != current_ch_` first |
| `free(psram_buf)` | `heap_caps_free(psram_buf)` |
| `portMAX_DELAY` on semaphore take | `pdMS_TO_TICKS(200)` + error handling |
| Single SPI mutex for multi-step TX | Add `radio_op_mutex_` for full sequence |
| `bool connected_` shared between tasks | `std::atomic<bool> connected_` |
| `esp_now_add_peer()` in WiFi event callback | Set atomic flag; handle in mesh task |
| `ulTaskNotifyTake(portMAX_DELAY)` | `pdMS_TO_TICKS(5000)` + watchdog log |
| `uint32_t t = esp_timer_get_time()` | `int64_t t = esp_timer_get_time()` |
| `vTaskDelay(50)` in processing loop | Reduce or replace with event-driven wait |
| Recursive `receive_fragment()` | Inline loop instead of recursion |
| `ESP_LOGW` in WiFi/ISR callback | Atomic counter; log from task |
| Polling `is_connected()` in task | `xEventGroupWaitBits` on connect event |
| `seq > neighbor.inet_seq` | `seq_newer(seq, neighbor.inet_seq)` |
| `memcpy(buf, data, len)` without bounds check | `if (len > MAX_MTU) return false` first |
| `file_size_ - offset` without index validation | Validate `data_idx < total_fragments_` first |
| `uint8_t buf[250]` on task stack | Move to class member or static global |
| `setUp()`/`tearDown()` in each test file | Define once in `test_main.cpp` only |
