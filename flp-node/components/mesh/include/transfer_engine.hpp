#pragma once

/*
 * =============================================================================
 * transfer_engine.hpp — File transfer state machine
 * 
 * Owns: ActiveTransfer, BroadcastRetry, exit-node election, SelectiveRepeat ARQ
 * Extracted from MeshManager to keep responsibilities focused.
 * =============================================================================
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "packet.hpp"
#include "fec_codec.hpp"
#include "selective_repeat.hpp"

namespace flp
{

/* Exit node election candidate */
struct ExitCandidate
{
    uint16_t addr;
    int8_t rssi_to_gw;
    uint8_t hops_to_gw;
};

/* Active file transfer state (sender side) */
struct ActiveTransfer
{
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint16_t fragment_count = 0;
    uint16_t fragment_size = 0;
    uint16_t next_fragment = 0;
    uint16_t exit_nodes[MAX_EXIT_NODES] = {};
    uint8_t exit_node_count = 0;
    uint16_t session_id = 0;
    char filename[20] = {};
    bool active = false;
    bool exit_node_alive[MAX_EXIT_NODES] = {true, true, true, true};
    uint32_t last_ack_ms[MAX_EXIT_NODES] = {};
};

/* Async broadcast retry state */
struct BroadcastRetry
{
    uint8_t payload[MAX_MTU];
    size_t payload_len = 0;
    PacketType type = PacketType::DISCOVERY;
    uint16_t dst_addr = BROADCAST_ADDR;
    uint8_t max_retries = 3;
    uint8_t attempt = 0;
    uint32_t next_send_ms = 0;
    uint32_t backoff_ms = 500;
    bool active = false;
};

/* Callback for sending packets (TransferEngine -> MeshManager) */
using SendPacketFn = std::function<void(uint16_t dst,
                                        PacketType type,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint16_t seq_num)>;

/* Callback for forwarding fragments to MQTT */
using ForwardToMqttFn = std::function<void(uint16_t session_id,
                                            uint16_t seq,
                                            uint16_t src_node,
                                            const uint8_t *data,
                                            size_t len,
                                            const char *filename)>;

/* Callback for publishing transfer meta to MQTT (exit node receives TRANSFER_AD) */
using ForwardMetaFn = std::function<void(uint16_t session_id,
                                          const char *filename,
                                          uint16_t src_node,
                                          uint32_t total_size,
                                          uint16_t chunk_count,
                                          uint16_t fragment_size,
                                          uint32_t crc32)>;

class TransferEngine
{
  public:
    TransferEngine() = default;

    void init(EventGroupHandle_t events,
              uint16_t my_addr,
              SendPacketFn send_fn);

    /* Packet handlers (called by MeshManager dispatch) */
    void handle_transfer_ad(const PacketHeader &hdr,
                            const uint8_t *payload,
                            size_t payload_len,
                            bool has_internet);
    void handle_transfer_ack(const PacketHeader &hdr,
                             const uint8_t *payload,
                             size_t payload_len);
    void handle_data(const PacketHeader &hdr,
                     const uint8_t *payload,
                     size_t payload_len);
    void handle_ack(uint16_t seq, uint16_t from_addr);
    void handle_nack(uint16_t seq, uint16_t from_addr);

    /*
     * Start a file transfer (sender side)
     * If has_internet && has_mqtt, uses local-exit fast path (no mesh).
     */
    void start_file_transfer(const char *filename,
                             const uint8_t *data,
                             size_t size,
                             bool has_internet = false,
                             bool has_mqtt = false,
                             uint8_t hops_to_internet = 0xFF);

    /* Periodic tick — call from MeshManager::run() */
    void tick(uint32_t now_ms);

    /* Access to transfer state for MQTT publish */
    const char *current_filename() const { return transfer_.filename; }
    bool is_transfer_active() const { return transfer_.active; }
    uint8_t get_progress_pct() const
    {
        if (!transfer_.active || transfer_.fragment_count == 0)
        {
            return 0;
        }
        return static_cast<uint8_t>(
            (transfer_.next_fragment * 100) / transfer_.fragment_count);
    }

    /* Exit node status */
    bool is_exit_node() const { return is_exit_node_; }

    /* Set callback for forwarding fragments to MQTT */
    void set_forward_to_mqtt(ForwardToMqttFn fn) { forward_to_mqtt_fn_ = fn; }

    /* Set callback for publishing transfer meta to MQTT (exit node) */
    void set_forward_meta(ForwardMetaFn fn) { forward_meta_fn_ = fn; }

  private:
    void transfer_tick();
    void broadcast_retry_tick(uint32_t now_ms);
    void election_timeout_tick(uint32_t now_ms);
    void exit_node_health_tick(uint32_t now_ms);
    void redistribute_dead_exit(uint8_t dead_idx);
    void send_broadcast_with_retry(PacketType type,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint16_t dst_addr = BROADCAST_ADDR,
                                   uint8_t max_retries = 3);
    int8_t arq_index_for_peer(uint16_t addr) const;

    static constexpr uint32_t EXIT_NODE_TIMEOUT_MS = 10000;
    uint32_t election_timeout_ms_ = 3000; /* Step 6: adaptive election window */

    SelectiveRepeat arq_[MAX_EXIT_NODES];
    ActiveTransfer transfer_ = {};
    BroadcastRetry broadcast_retry_ = {};

    std::array<ExitCandidate, 4> candidates_ = {};
    uint8_t candidate_count_ = 0;
    uint32_t election_start_ms_ = 0;
    bool election_active_ = false;

    EventGroupHandle_t events_ = nullptr;
    uint16_t my_addr_ = 0;
    SendPacketFn send_fn_;

    FecEncoder fec_encoder_;

    ForwardToMqttFn forward_to_mqtt_fn_;
    ForwardMetaFn forward_meta_fn_;
    bool is_exit_node_ = false;
    bool local_exit_ = false;
    uint16_t active_session_id_ = 0;
    uint16_t source_addr_ = 0;

    /* Deferred meta: exit node stores ad info until fragment 0 delivers filename */
    struct PendingMeta
    {
        uint32_t file_size;
        uint16_t fragment_count;
        uint16_t fragment_size;
        uint16_t crc16;
        bool waiting; /* true = waiting for frag 0 with filename */
    } pending_meta_ = {};

    /* Pending redistribution queue (fragments from dead exit nodes) */
    uint16_t redist_pending_[ARQ_WINDOW * MAX_EXIT_NODES] = {};
    uint8_t redist_count_ = 0;
};

} /* namespace flp */
