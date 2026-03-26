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

/* Callback for reading a chunk of file data on demand */
using ReadChunkFn = std::function<size_t(uint8_t *buf, size_t offset, size_t len)>;

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
    ReadChunkFn read_chunk;
    size_t size = 0;
    uint16_t fragment_count = 0;
    uint16_t fragment_size = 0;
    uint16_t next_fragment = 0;
    uint16_t exit_nodes[MAX_EXIT_NODES] = {};
    uint8_t exit_node_count = 0;
    uint16_t session_id = 0;
    char filename[33] = {}; /* 32 chars + NUL (matches wire format) */
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

/* Callback for forwarding fragments to MQTT (returns true if queued OK) */
using ForwardToMqttFn = std::function<bool(uint16_t session_id,
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

/* Callback for draining ACK/NACK from MqttClient */
using DrainSeqFn = std::function<bool(uint16_t &seq_out)>;

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
     * Start a file transfer (sender side).
     * If has_internet && has_mqtt, uses local-exit fast path (no mesh).
     * Default hops_to_internet=1 (single hop); callers should pass the
     * actual hop count from RouteTable.
     */
    void start_file_transfer(const char *filename,
                             size_t size,
                             ReadChunkFn read_chunk,
                             bool has_internet = false,
                             bool has_mqtt = false,
                             uint8_t hops_to_internet = 1);

    /* Helper: wrap a contiguous buffer as a ReadChunkFn */
    static ReadChunkFn make_buffer_reader(const uint8_t *data, size_t size);

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
        uint16_t progress_seq = local_exit_ ? cloud_base_seq_
                                            : transfer_.next_fragment;
        return static_cast<uint8_t>(
            (progress_seq * 100) / transfer_.fragment_count);
    }

    /* Exit node status */
    bool is_exit_node() const { return is_exit_node_; }

    /* Active session ID (0 if no transfer in progress) */
    uint16_t active_session_id() const
    {
        if (transfer_.active) return transfer_.session_id;
        if (is_exit_node_)    return active_session_id_;
        return 0;
    }

    /* Called when an exit node reports itself offline */
    void handle_exit_offline(uint16_t exit_addr, uint16_t session_id);

    /* Called when a relay signals congestion via the ACK/NACK high bit.
     * Causes transfer_tick() to skip one cycle of fragment feeding. */
    void signal_congestion() { congestion_backoff_ticks_++; }

    /* Set callback for forwarding fragments to MQTT */
    void set_forward_to_mqtt(ForwardToMqttFn fn) { forward_to_mqtt_fn_ = fn; }

    /* Set callback for publishing transfer meta to MQTT (exit node) */
    void set_forward_meta(ForwardMetaFn fn) { forward_meta_fn_ = fn; }

    /* Set drain callbacks for cloud ACK/NACK (local-exit selective repeat) */
    void set_cloud_ack_drain(DrainSeqFn fn) { drain_cloud_ack_fn_ = fn; }
    void set_cloud_nack_drain(DrainSeqFn fn) { drain_cloud_nack_fn_ = fn; }

  private:
    void transfer_tick();
    void tick_local_exit_arq();
    void tick_mesh_arq();
    void compact_retx_queue(uint8_t sent);
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
    static constexpr uint32_t MAX_ARQ_TIMEOUT_MS = 3000;
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
    DrainSeqFn drain_cloud_ack_fn_;
    DrainSeqFn drain_cloud_nack_fn_;
    bool is_exit_node_ = false;
    bool local_exit_ = false;
    uint16_t active_session_id_ = 0;
    uint16_t source_addr_ = 0;

    /* Reusable scratch buffer for fragment I/O (avoids 4x stack alloc) */
    uint8_t frag_buf_[MAX_MTU] = {};

    /* Cloud selective-repeat ARQ state (local-exit path) */
    static constexpr uint8_t CLOUD_WINDOW_SIZE = 8;
    static constexpr uint16_t MAX_CLOUD_FRAGMENTS = 2200;
    static constexpr uint16_t CLOUD_ACK_BITMAP_BYTES =
        (MAX_CLOUD_FRAGMENTS + 7) / 8;  /* 275 bytes */
    uint8_t cloud_ack_bitmap_[CLOUD_ACK_BITMAP_BYTES] = {};
    uint16_t cloud_base_seq_ = 0;       /* lowest un-ACK'd seq */
    uint16_t cloud_next_send_ = 0;      /* next seq to send for first time */

    /* Retransmit queue for NACK'd fragments */
    static constexpr uint8_t CLOUD_RETX_QUEUE_SIZE = 16;
    uint16_t cloud_retx_queue_[CLOUD_RETX_QUEUE_SIZE] = {};
    uint8_t cloud_retx_count_ = 0;

    /* Pending redistribution queue (fragments from dead exit nodes) */
    uint16_t redist_pending_[ARQ_WINDOW * MAX_EXIT_NODES] = {};
    uint8_t redist_count_ = 0;

    /* Congestion backoff: each signal_congestion() call adds one skip tick */
    uint8_t congestion_backoff_ticks_ = 0;
};

} /* namespace flp */
