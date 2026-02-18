#pragma once

// =============================================================================
// transfer_engine.hpp — File transfer state machine
//
// Owns: ActiveTransfer, BroadcastRetry, exit-node election, SelectiveRepeat ARQ
// Extracted from MeshManager to keep responsibilities focused.
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "packet.hpp"
#include "protocol_selector.hpp"
#include "selective_repeat.hpp"

namespace flp
{

// Exit node election candidate
struct ExitCandidate
{
    uint16_t addr;
    int8_t rssi_to_gw;
    uint8_t hops_to_gw;
};

// Active file transfer state (sender side)
struct ActiveTransfer
{
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint16_t fragment_count = 0;
    uint16_t fragment_size = 0;
    uint16_t next_fragment = 0;
    uint16_t exit_node = 0;
    char filename[20] = {};
    bool active = false;
};

// Async broadcast retry state
struct BroadcastRetry
{
    uint8_t payload[MAX_MTU];
    size_t payload_len = 0;
    PacketType type = PacketType::DISCOVERY;
    uint8_t max_retries = 3;
    uint8_t attempt = 0;
    uint32_t next_send_ms = 0;
    uint32_t backoff_ms = 500;
    bool active = false;
};

// Callback for sending packets (TransferEngine -> MeshManager)
using SendPacketFn = std::function<void(uint16_t dst,
                                        PacketType type,
                                        const uint8_t *payload,
                                        size_t payload_len)>;

// Callback for sending raw bytes with transport selection
using SendRawFn = std::function<void(Transport transport,
                                     const uint8_t *data,
                                     size_t len,
                                     uint16_t peer_addr)>;

// Callback for protocol selection
using SelectTransportFn = std::function<Transport(int8_t rssi,
                                                  uint8_t hops,
                                                  size_t payload_size)>;

class TransferEngine
{
  public:
    TransferEngine() = default;

    void init(EventGroupHandle_t events,
              uint16_t my_addr,
              SendPacketFn send_fn,
              SendRawFn send_raw_fn,
              SelectTransportFn select_fn);

    // Packet handlers (called by MeshManager dispatch)
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
    void handle_ack(uint16_t seq);
    void handle_nack(uint16_t seq);

    // Start a file transfer (sender side)
    void start_file_transfer(const char *filename,
                             const uint8_t *data,
                             size_t size,
                             Transport preferred_transport);

    // Periodic tick — call from MeshManager::run()
    void tick(uint32_t now_ms);

    // Access to transfer state for MQTT publish
    const char *current_filename() const { return transfer_.filename; }
    bool is_transfer_active() const { return transfer_.active; }

    // ARQ passthrough for receiver buffer access
    const uint8_t *get_reassembly_buffer() const
    {
        return arq_.get_reassembly_buffer();
    }
    size_t get_file_size() const { return arq_.get_file_size(); }
    bool is_receive_complete() const { return arq_.is_complete(); }
    void cleanup_receiver() { arq_.cleanup_receiver(); }

  private:
    void transfer_tick();
    void broadcast_retry_tick(uint32_t now_ms);
    void election_timeout_tick(uint32_t now_ms);
    void send_broadcast_with_retry(PacketType type,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   uint8_t max_retries = 3);

    SelectiveRepeat arq_;
    ActiveTransfer transfer_ = {};
    BroadcastRetry broadcast_retry_ = {};

    std::array<ExitCandidate, 4> candidates_ = {};
    uint8_t candidate_count_ = 0;
    uint32_t election_start_ms_ = 0;
    bool election_active_ = false;

    // Receiver-side PSRAM cleanup timeout
    uint32_t receiver_init_ms_ = 0;
    bool receiver_waiting_ = false;

    EventGroupHandle_t events_ = nullptr;
    uint16_t my_addr_ = 0;
    SendPacketFn send_fn_;
    SendRawFn send_raw_fn_;
    SelectTransportFn select_fn_;
};

} // namespace flp
