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
    uint8_t active_transfers;
};

/* Per-exit-path quality statistics for weighted scheduling (Phase 3).
 * Updated from ACK/NACK handling; used by tick_mesh_arq() to select
 * the best exit node for each fragment. */
struct ExitPathStats
{
    float ewma_rtt_ms   = 500.0f; /* EWMA of round-trip time */
    float ewma_loss     = 0.0f;   /* EWMA of loss rate [0,1] */
    uint32_t sent       = 0;
    uint32_t acked      = 0;
    uint32_t nacked     = 0;
    float weight        = 1.0f;   /* normalized scheduling weight */
};

/* Active file transfer state (sender side) */
struct ActiveTransfer
{
    ReadChunkFn read_chunk;
    size_t size = 0;
    uint32_t crc32 = 0; /* stored for periodic meta re-publish */
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

/* Callback for sending packets (TransferEngine -> MeshManager).
 * Returns 0 on success, -1 on failure (e.g. ESP_ERR_ESPNOW_NO_MEM).
 * Callers can use the return value to stop sending when the radio
 * TX buffer is exhausted. */
using SendPacketFn = std::function<int(uint16_t dst,
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
using DrainSeqFn = std::function<bool(uint16_t session_id, uint16_t &seq_out)>;

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
    bool start_file_transfer(const char *filename,
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

    /* Called when a relay/exit signals congestion via the ACK/NACK high bit.
     * Adds 4 skip-ticks per signal (~40-80ms pause), capped at 20 ticks
     * to avoid stalling the transfer while giving the exit node drain time. */
    void signal_congestion()
    {
        total_congestion_events_++;
        congestion_backoff_ticks_ += 2;
        if (congestion_backoff_ticks_ > 12) { congestion_backoff_ticks_ = 12; }
    }
    void note_local_backpressure()
    {
        total_local_backpressure_events_++;
        signal_congestion();
    }

    /* Set callback for forwarding fragments to MQTT */
    void set_forward_to_mqtt(ForwardToMqttFn fn) { forward_to_mqtt_fn_ = fn; }

    /* Set callback for publishing transfer meta to MQTT (exit node) */
    void set_forward_meta(ForwardMetaFn fn) { forward_meta_fn_ = fn; }

    /* Set drain callbacks for cloud ACK/NACK (local-exit selective repeat) */
    void set_cloud_ack_drain(DrainSeqFn fn) { drain_cloud_ack_fn_ = fn; }
    void set_cloud_nack_drain(DrainSeqFn fn) { drain_cloud_nack_fn_ = fn; }

    using RequeueSeqFn = std::function<void(uint16_t session_id, uint16_t seq)>;
    void set_cloud_nack_requeue(RequeueSeqFn fn) { requeue_cloud_nack_fn_ = fn; }
    void set_cloud_nack_observer(RequeueSeqFn fn) { observe_cloud_nack_fn_ = fn; }

    /* Set drain callback for deferred fragment ACKs (exit node: MQTT published OK) */
    void set_fragment_ack_drain(DrainSeqFn fn) { drain_fragment_ack_fn_ = fn; }
    void set_fragment_ack_requeue(RequeueSeqFn fn) { requeue_fragment_ack_fn_ = fn; }

    /* Session consensus: cloud confirmed transfer complete.
     * Set a callback that returns true (once) when the cloud has
     * published TRANSFER_COMPLETE.  The exit node polls this each tick
     * and forwards TRANSFER_DONE to the source via mesh. */
    using TransferCompleteFn = std::function<bool(uint16_t session_id)>;
    void set_transfer_complete_fn(TransferCompleteFn fn)
    {
        transfer_complete_fn_ = fn;
    }

    using SessionLifecycleFn = std::function<void(uint16_t session_id)>;
    void set_mqtt_session_end_fn(SessionLifecycleFn fn)
    {
        mqtt_session_end_fn_ = fn;
    }

    /* Handle TRANSFER_DONE from exit node (source side) */
    void handle_transfer_done(uint16_t sender,
                              const uint8_t *payload,
                              size_t payload_len);

  private:
    static uint8_t sanitize_hops_to_exit(uint8_t hops);
    static uint32_t compute_election_timeout_ms(uint8_t hops_to_exit);
    static uint32_t compute_arq_timeout_ms(uint8_t hops_to_exit);
    static uint32_t compute_min_rto_floor_ms(uint8_t hops_to_exit);
    static uint8_t compute_mesh_window_cap(uint8_t hops_to_exit);
    static uint32_t compute_exit_timeout_ms(uint8_t hops_to_exit);
    uint32_t total_acked_fragments() const;
    uint32_t data_fragments_before_seq(uint16_t seq) const;
    size_t bytes_from_seq_progress(uint16_t seq) const;
    void reset_telemetry(uint32_t now_ms);
    bool ensure_telemetry_csv_buffer();
    void append_telemetry_csv_row(const char *result,
                                  uint32_t now_ms,
                                  size_t sent_bytes,
                                  uint32_t tx_Bps,
                                  uint32_t acked_frags,
                                  uint32_t ack_fps,
                                  uint16_t inflight,
                                  uint32_t mesh_ms = 0,
                                  uint32_t cloud_wait_ms = 0,
                                  uint32_t e2e_Bps = 0,
                                  uint32_t mesh_Bps = 0);
    void flush_telemetry_csv();
    void log_transfer_summary(const char *result, uint32_t now_ms);

    void transfer_tick();
    void tick_local_exit_arq();
    void tick_mesh_arq();
    void compact_retx_queue(uint8_t sent);
    void reset_sender_transfer_state(bool signal_complete);
    void clear_exit_node_state();
    void broadcast_retry_tick(uint32_t now_ms);
    void election_timeout_tick(uint32_t now_ms);
    void exit_node_health_tick(uint32_t now_ms);
    void redistribute_dead_exit(uint8_t dead_idx);
    void recompute_weights();
    int8_t select_mesh_target_for_seq(uint16_t seq) const;
    uint16_t total_mesh_inflight() const;
    uint8_t mesh_window_cap_per_exit() const;
    void log_transfer_diag(uint32_t now_ms, uint8_t mesh_retx_used);
    void send_broadcast_with_retry(PacketType type,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint16_t dst_addr = BROADCAST_ADDR,
                                   uint8_t max_retries = 3);
    int8_t arq_index_for_peer(uint16_t addr) const;
    int8_t arq_index_for_sequence(uint16_t seq) const;

    static constexpr uint32_t BASE_ARQ_TIMEOUT_MS = 3000;
    static constexpr uint32_t PER_HOP_ARQ_TIMEOUT_MS = 1000;
    static constexpr uint32_t MAX_ARQ_TIMEOUT_MS = 8000;
    static constexpr uint32_t MULTIHOP_MIN_RTO_FLOOR_MS = 1200;
    static constexpr uint32_t MAX_MIN_RTO_FLOOR_MS = 2000;
    static constexpr uint32_t BASE_ELECTION_MS = 3000;
    static constexpr uint32_t PER_HOP_ELECTION_MS = 1250;
    static constexpr uint32_t MAX_ELECTION_MS = 9000;
    static constexpr uint32_t BASE_EXIT_NODE_TIMEOUT_MS = 30000;
    static constexpr uint32_t PER_HOP_EXIT_NODE_TIMEOUT_MS = 5000;
    static constexpr uint32_t MAX_EXIT_NODE_TIMEOUT_MS = 60000;
    static constexpr uint8_t TWO_HOP_MESH_WINDOW_CAP = 24;
    static constexpr uint8_t MULTIHOP_MESH_WINDOW_CAP = 16;
    static constexpr uint8_t MIN_PER_EXIT_WINDOW_CAP = 8;
    uint32_t election_timeout_ms_ = 3000; /* Step 6: adaptive election window */
    uint32_t exit_node_timeout_ms_ = BASE_EXIT_NODE_TIMEOUT_MS;
    uint8_t source_hops_to_exit_est_ = 1;
    uint8_t mesh_window_cap_ = ARQ_WINDOW;

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
    DrainSeqFn drain_fragment_ack_fn_;
    RequeueSeqFn requeue_cloud_nack_fn_;
    RequeueSeqFn observe_cloud_nack_fn_;
    RequeueSeqFn requeue_fragment_ack_fn_;
    TransferCompleteFn transfer_complete_fn_;
    SessionLifecycleFn mqtt_session_end_fn_;
    bool is_exit_node_ = false;
    bool exit_pending_ = false;
    bool local_exit_ = false;
    uint16_t active_session_id_ = 0;
    uint16_t source_addr_ = 0;

    /* Reusable scratch buffer for fragment I/O (avoids 4x stack alloc) */
    uint8_t frag_buf_[MAX_MTU] = {};

    /* Cloud selective-repeat ARQ state (local-exit path) */
    static constexpr uint8_t CLOUD_WINDOW_SIZE = 16;
    static constexpr uint16_t MAX_CLOUD_FRAGMENTS = 8192;
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
    uint16_t redist_count_ = 0;

    /* Out-of-window retransmit queue: cloud NACKs for seqs the ARQ has
     * already released.  Route to the exit that requested the resend. */
    static constexpr uint16_t OOW_RETX_QUEUE_SIZE = 1024;
    uint16_t oow_retx_queue_[OOW_RETX_QUEUE_SIZE] = {};
    uint16_t oow_retx_dst_[OOW_RETX_QUEUE_SIZE] = {};
    uint16_t oow_retx_count_ = 0;
    uint32_t last_oow_retx_ms_ = 0;

    /* Congestion backoff: each signal_congestion() call adds one skip tick */
    uint8_t congestion_backoff_ticks_ = 0;

    /* Cloud ACK stall detection for local-exit and exit-node timeouts.
     * If no cloud ACK arrives within this period, abort the transfer so
     * auto-demo or a new TRANSFER_AD can proceed. */
    static constexpr uint32_t CLOUD_STALL_TIMEOUT_MS = 30000;
    uint32_t last_cloud_activity_ms_ = 0;
    uint32_t last_mesh_frag_send_ms_ = 0;
    bool mesh_upload_done_ = false;
    uint32_t transfer_start_ms_ = 0;
    uint32_t mesh_upload_done_ms_ = 0;

    /* Periodic meta re-publish: if the cloud restarts mid-transfer, it has
     * no session state. Re-publishing meta lets it pick up the session. */
    static constexpr uint32_t META_REPUBLISH_INTERVAL_MS = 10000;
    uint32_t last_meta_publish_ms_ = 0;
    uint32_t last_diag_log_ms_ = 0;
    uint32_t last_telemetry_log_ms_ = 0;
    size_t last_telemetry_sent_bytes_ = 0;
    uint32_t last_telemetry_acked_frags_ = 0;

    /* Phase 3: per-exit-path quality stats for weighted scheduling */
    ExitPathStats path_stats_[MAX_EXIT_NODES] = {};
    uint16_t weight_recompute_counter_ = 0;
    static constexpr uint16_t WEIGHT_RECOMPUTE_INTERVAL = 16; /* every N frags */

    /* Evaluation telemetry counters */
    uint32_t total_mesh_retx_sent_ = 0;
    uint32_t total_oow_retx_queued_ = 0;
    uint32_t total_oow_retx_sent_ = 0;
    uint32_t total_congestion_events_ = 0;
    uint32_t total_local_backpressure_events_ = 0;
    uint16_t max_total_inflight_ = 0;

    /* Buffered CSV telemetry sink (flushed to SD at transfer end) */
    static constexpr size_t TELEMETRY_CSV_BUFFER_BYTES = 32 * 1024;
    char *telemetry_csv_buf_ = nullptr;
    size_t telemetry_csv_len_ = 0;
    bool telemetry_csv_dropped_ = false;
    bool telemetry_summary_written_ = false;
    char telemetry_csv_path_[48] = {};
};

} /* namespace flp */
