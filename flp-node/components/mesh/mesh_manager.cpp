#include "mesh_manager.hpp"
#include "mqtt_sn_client.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "mesh_mgr";

namespace flp {

// ── Task 1: WiFi status setter ──────────────────────────────────────────────

void MeshManager::set_has_internet(bool v)
{
    if (has_internet_ != v) {
        has_internet_ = v;
        ESP_LOGI(TAG, "has_internet_ = %s", v ? "true" : "false");
    }
}

// ── Init ─────────────────────────────────────────────────────────────────────

void MeshManager::init()
{
    // Derive node address from MAC (lower 16 bits)
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    my_addr_ = static_cast<uint16_t>((mac[4] << 8) | mac[5]);
    ESP_LOGI(TAG, "Node addr: 0x%04X", my_addr_);

    // Create unified inbound packet queue (16 slots)
    packet_queue_ = xQueueCreate(16, sizeof(RxPacket));
    assert(packet_queue_);

    // Create event group
    events_ = xEventGroupCreate();
    assert(events_);

    // Init transports
    ble_.init();
    lora_.init(lora_rx_priority_);

    // Start BLE advertising + scanning
    ble_.start_advertise();
    ble_.start_scan();

    // Init protocol selector
    protocol_selector_.init();

    // Init ARQ
    arq_.init(ARQ_WINDOW, ARQ_TIMEOUT);
    arq_.set_send_callback(
        [this](uint16_t dst, PacketType type, uint16_t seq,
               const uint8_t *data, size_t len) {
            uint8_t buf[MAX_MTU];
            PacketHeader hdr = {};
            hdr.set_ver_type(PROTOCOL_VERSION, type);
            hdr.src_addr = my_addr_;
            hdr.dst_addr = dst;
            hdr.set_ttl_hops(DEFAULT_TTL, 0);
            hdr.seq_num = seq;

            memcpy(buf, &hdr, PACKET_HEADER_SIZE);
            if (data && len > 0) {
                memcpy(buf + PACKET_HEADER_SIZE, data, len);
            }

            size_t total = PACKET_HEADER_SIZE + len;
            Transport t = protocol_selector_.select(0, 0, total, 100.0f);
            send_raw(t, buf, total, dst);
        });

    // BLE callback: items go via BLE rx_queue and are polled in run()
    ble_.on_receive([](uint16_t src_addr, const uint8_t *data, size_t len) {
        (void)src_addr; (void)data; (void)len;
    });

    // LoRa callback similarly uses its own rx_queue
    lora_.on_receive([](const uint8_t *data, size_t len, int rssi) {
        (void)data; (void)len; (void)rssi;
    });

    discovery_timer_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "MeshManager initialized");
}

// ── Main loop ────────────────────────────────────────────────────────────────

void MeshManager::run()
{
    while (true) {
        // Poll BLE rx_queue and forward to unified packet_queue_
        BleRxItem ble_item;
        while (xQueueReceive(ble_.get_rx_queue(), &ble_item, 0) == pdTRUE) {
            RxPacket rpkt = {};
            size_t copy_len = ble_item.len;
            if (copy_len > MAX_MTU) copy_len = MAX_MTU;
            memcpy(rpkt.data, ble_item.data, copy_len);
            rpkt.len = copy_len;
            rpkt.rssi = ble_.get_peer_rssi(ble_item.src_addr);
            rpkt.source = RxTransport::BLE;
            xQueueSend(packet_queue_, &rpkt, 0);
        }

        // Poll LoRa rx_queue
        LoraRxItem lora_item;
        while (xQueueReceive(lora_.get_rx_queue(), &lora_item, 0) == pdTRUE) {
            RxPacket rpkt = {};
            size_t copy_len = lora_item.len;
            if (copy_len > MAX_MTU) copy_len = MAX_MTU;
            memcpy(rpkt.data, lora_item.data, copy_len);
            rpkt.len = copy_len;
            rpkt.rssi = static_cast<int8_t>(lora_item.rssi);
            rpkt.source = RxTransport::LORA;
            xQueueSend(packet_queue_, &rpkt, 0);
        }

        // Process unified queue — short timeout to keep periodic tasks responsive
        RxPacket pkt;
        if (xQueueReceive(packet_queue_, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            process_packet(pkt);
        }

        // Periodic tasks
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        // Discovery broadcast every 10 seconds
        if (now - discovery_timer_ms_ > 10000) {
            send_discovery();
            discovery_timer_ms_ = now;
        }

        // Prune stale neighbors (30s timeout)
        route_table_.prune_stale(30000);

        // ARQ tick for timeout retransmits
        arq_.tick();

        // Task 4: Election timeout check
        if (election_active_ && (now - election_start_ms_ > 3000)) {
            election_active_ = false;

            if (candidate_count_ == 0) {
                ESP_LOGW(TAG, "Election timeout: no exit node candidates");
                transfer_.active = false;
            } else {
                // Pick best: lowest hops_to_gw, then best rssi_to_gw as tiebreaker
                uint8_t best_idx = 0;
                for (uint8_t i = 1; i < candidate_count_; i++) {
                    if (candidates_[i].hops_to_gw < candidates_[best_idx].hops_to_gw ||
                        (candidates_[i].hops_to_gw == candidates_[best_idx].hops_to_gw &&
                         candidates_[i].rssi_to_gw > candidates_[best_idx].rssi_to_gw)) {
                        best_idx = i;
                    }
                }

                transfer_.exit_node = candidates_[best_idx].addr;
                ESP_LOGI(TAG, "Elected exit node: 0x%04X (hops=%u rssi=%d)",
                         transfer_.exit_node, candidates_[best_idx].hops_to_gw,
                         candidates_[best_idx].rssi_to_gw);

                xEventGroupSetBits(events_, FLP_EVT_EXIT_NODE_ELECTED);

                // Begin fragment transfer
                arq_.reset_sender();
                arq_.set_peer_addr(transfer_.exit_node);
                transfer_.next_fragment = 0;
            }
        }

        // Task 5: Feed fragments into ARQ window
        transfer_tick();
    }
}

// ── Packet dispatch ──────────────────────────────────────────────────────────

void MeshManager::process_packet(const RxPacket &pkt)
{
    if (pkt.len < PACKET_HEADER_SIZE) {
        ESP_LOGW(TAG, "Packet too short: %zu bytes", pkt.len);
        return;
    }

    PacketHeader hdr;
    memcpy(&hdr, pkt.data, PACKET_HEADER_SIZE);

    if (hdr.version() != PROTOCOL_VERSION) {
        ESP_LOGW(TAG, "Unknown version %u", hdr.version());
        return;
    }

    if (hdr.ttl() == 0) {
        ESP_LOGD(TAG, "Dropping packet, TTL=0");
        return;
    }

    // Update route table with source info
    bool via_ble = (pkt.source == RxTransport::BLE);
    bool via_lora = (pkt.source == RxTransport::LORA);
    route_table_.update_neighbor(hdr.src_addr, pkt.rssi, hdr.hop_count(),
                                  via_ble, via_lora);

    const uint8_t *payload = pkt.data + PACKET_HEADER_SIZE;
    size_t payload_len = pkt.len - PACKET_HEADER_SIZE;

    // Is this packet for us or broadcast?
    if (hdr.dst_addr == my_addr_ || hdr.dst_addr == BROADCAST_ADDR) {
        switch (hdr.type()) {
        case PacketType::DISCOVERY:
            handle_discovery(hdr, payload, payload_len);
            break;
        case PacketType::TRANSFER_AD:
            handle_transfer_ad(hdr, payload, payload_len);
            break;
        case PacketType::TRANSFER_ACK:
            handle_transfer_ack(hdr, payload, payload_len);
            break;
        case PacketType::DATA:
            handle_data(hdr, payload, payload_len);
            break;
        case PacketType::ACK:
            arq_.handle_ack(hdr.seq_num);
            break;
        case PacketType::NACK:
            arq_.handle_nack(hdr.seq_num);
            break;
        default:
            ESP_LOGD(TAG, "Unhandled packet type 0x%02X",
                     static_cast<uint8_t>(hdr.type()));
            break;
        }
    }

    // Forward if not for us (and not broadcast)
    if (hdr.dst_addr != my_addr_ && hdr.dst_addr != BROADCAST_ADDR) {
        forward_packet(pkt, hdr);
    }
}

// ── Task 3: Discovery with hops_to_internet tracking ─────────────────────────

void MeshManager::handle_discovery(const PacketHeader &hdr, const uint8_t *payload,
                                   size_t payload_len)
{
    if (payload_len < sizeof(DiscoveryPayload)) return;

    DiscoveryPayload disc;
    memcpy(&disc, payload, sizeof(disc));

    ESP_LOGI(TAG, "Discovery from 0x%04X: inet=%u hops_inet=%u rssi=%d",
             hdr.src_addr, disc.flags & 0x01, disc.hops_to_internet, disc.rssi);

    if (disc.flags & 0x01) {
        route_table_.set_has_internet(hdr.src_addr, true);
    }

    // Store hops_to_internet from discovery payload into route table
    route_table_.update_neighbor(hdr.src_addr, disc.rssi, 1,
                                  false, true, disc.hops_to_internet);

    // Respond with our own discovery (unicast back)
    DiscoveryPayload resp = {};
    resp.flags = has_internet_ ? 0x01 : 0x00;

    if (has_internet_) {
        resp.hops_to_internet = 0;
    } else {
        uint8_t min_h = route_table_.min_hops_to_internet();
        resp.hops_to_internet = (min_h < 0xFE) ? static_cast<uint8_t>(min_h + 1) : 0xFF;
    }
    resp.rssi = 0;

    send_packet(hdr.src_addr, PacketType::DISCOVERY,
                reinterpret_cast<const uint8_t *>(&resp), sizeof(resp));
}

// ── Task 4: Transfer ad handling ─────────────────────────────────────────────

void MeshManager::handle_transfer_ad(const PacketHeader &hdr, const uint8_t *payload,
                                     size_t payload_len)
{
    if (payload_len < sizeof(TransferAdPayload)) return;

    TransferAdPayload ad;
    memcpy(&ad, payload, sizeof(ad));

    ESP_LOGI(TAG, "Transfer ad from 0x%04X: file=%s size=%u frags=%u",
             hdr.src_addr, ad.filename, ad.file_size, ad.fragment_count);

    // If we have internet, respond as exit node candidate
    if (has_internet_) {
        TransferAckPayload ack = {};
        ack.exit_node_addr = my_addr_;
        ack.hops_to_gw = 0; // Direct internet
        ack.rssi_to_gw = 0;

        send_packet(hdr.src_addr, PacketType::TRANSFER_ACK,
                    reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));

        // Prepare receiver for incoming file transfer
        arq_.init_receiver(ad.fragment_count, ad.fragment_size, ad.file_size);
        arq_.set_peer_addr(hdr.src_addr);

        ESP_LOGI(TAG, "Responded as exit node candidate to 0x%04X", hdr.src_addr);
    }
}

// ── Task 4: Transfer ACK handling (exit node election) ───────────────────────

void MeshManager::handle_transfer_ack(const PacketHeader &hdr, const uint8_t *payload,
                                       size_t payload_len)
{
    if (payload_len < sizeof(TransferAckPayload)) return;
    if (!election_active_) return;

    TransferAckPayload ack;
    memcpy(&ack, payload, sizeof(ack));

    if (candidate_count_ < candidates_.size()) {
        candidates_[candidate_count_] = {
            ack.exit_node_addr,
            ack.rssi_to_gw,
            ack.hops_to_gw
        };
        candidate_count_++;
        ESP_LOGI(TAG, "Exit candidate #%u: 0x%04X hops=%u rssi=%d",
                 candidate_count_, ack.exit_node_addr, ack.hops_to_gw, ack.rssi_to_gw);
    }
}

// ── Data handling (receiver side) ────────────────────────────────────────────

void MeshManager::handle_data(const PacketHeader &hdr, const uint8_t *payload,
                              size_t payload_len)
{
    arq_.set_peer_addr(hdr.src_addr);
    arq_.receive_fragment(hdr.seq_num, payload, payload_len);

    if (arq_.is_complete()) {
        ESP_LOGI(TAG, "File reassembly complete (%zu bytes)", arq_.get_file_size());
        xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);

        // Task 6: Hand reassembled file to MQTT for cloud upload
        if (mqtt_client_ && has_internet_) {
            mqtt_client_->publish_file(
                transfer_.filename[0] ? transfer_.filename : "unknown",
                arq_.get_reassembly_buffer(),
                arq_.get_file_size(),
                hdr.src_addr);
        }
    }
}

// ── Forwarding ───────────────────────────────────────────────────────────────

void MeshManager::forward_packet(const RxPacket &pkt, const PacketHeader &hdr)
{
    uint16_t next = route_table_.next_hop(hdr.dst_addr);
    if (next == BROADCAST_ADDR) {
        ESP_LOGW(TAG, "No route to 0x%04X, dropping", hdr.dst_addr);
        return;
    }

    uint8_t buf[MAX_MTU];
    memcpy(buf, pkt.data, pkt.len);

    PacketHeader *fwd_hdr = reinterpret_cast<PacketHeader *>(buf);
    fwd_hdr->set_ttl_hops(fwd_hdr->ttl() - 1, fwd_hdr->hop_count() + 1);

    NeighborEntry neighbor;
    int8_t rssi = -90;
    if (route_table_.get_neighbor(next, neighbor)) {
        rssi = neighbor.rssi;
    }

    Transport t = protocol_selector_.select(rssi, fwd_hdr->hop_count(),
                                             pkt.len, 100.0f);

    ESP_LOGD(TAG, "Forwarding to 0x%04X via 0x%04X (%s), ttl=%u",
             hdr.dst_addr, next,
             (t == Transport::BLE) ? "BLE" : "LoRa",
             fwd_hdr->ttl());

    send_raw(t, buf, pkt.len, next);
}

// ── Task 3: Discovery broadcast with computed hops_to_internet ───────────────

void MeshManager::send_discovery()
{
    DiscoveryPayload disc = {};
    disc.flags = has_internet_ ? 0x01 : 0x00;

    if (has_internet_) {
        disc.hops_to_internet = 0;
    } else {
        uint8_t min_h = route_table_.min_hops_to_internet();
        disc.hops_to_internet = (min_h < 0xFE) ? static_cast<uint8_t>(min_h + 1) : 0xFF;
    }
    disc.rssi = 0;

    // Task 2: Use broadcast retry instead of single send
    send_broadcast_with_retry(PacketType::DISCOVERY,
                              reinterpret_cast<const uint8_t *>(&disc), sizeof(disc));

    ESP_LOGD(TAG, "Sent discovery broadcast (hops_to_inet=%u)", disc.hops_to_internet);
}

// ── Task 2: Broadcast with retry (exponential backoff) ───────────────────────

void MeshManager::send_broadcast_with_retry(PacketType type, const uint8_t *payload,
                                            size_t payload_len, uint8_t max_retries)
{
    uint32_t backoff_ms = 500;

    for (uint8_t attempt = 0; attempt < max_retries; attempt++) {
        send_packet(BROADCAST_ADDR, type, payload, payload_len);

        if (attempt + 1 < max_retries) {
            ESP_LOGD(TAG, "Broadcast retry %u/%u, backoff %ums",
                     attempt + 1, max_retries, backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
        }
    }
}

// ── Task 5: Start file transfer (sender side) ───────────────────────────────

void MeshManager::start_file_transfer(const char *filename, const uint8_t *data,
                                       size_t size)
{
    if (transfer_.active) {
        ESP_LOGW(TAG, "Transfer already in progress");
        return;
    }

    // Determine fragment size based on preferred transport
    Transport t = protocol_selector_.select(0, 0, size, 100.0f);
    size_t frag_payload = (t == Transport::BLE) ? BLE_MAX_PAYLOAD : LORA_MAX_PAYLOAD;

    transfer_.data = data;
    transfer_.size = size;
    transfer_.fragment_size = static_cast<uint16_t>(frag_payload);
    transfer_.fragment_count = static_cast<uint16_t>((size + frag_payload - 1) / frag_payload);
    transfer_.next_fragment = 0;
    transfer_.active = true;
    strncpy(transfer_.filename, filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    ESP_LOGI(TAG, "Starting file transfer: %s (%zu bytes, %u fragments)",
             filename, size, transfer_.fragment_count);

    // Broadcast TRANSFER_AD with retry to discover exit nodes
    TransferAdPayload ad = {};
    ad.file_size = static_cast<uint32_t>(size);
    ad.fragment_count = transfer_.fragment_count;
    ad.fragment_size = transfer_.fragment_size;
    strncpy(ad.filename, filename, sizeof(ad.filename) - 1);

    // Start election
    candidate_count_ = 0;
    election_active_ = true;
    election_start_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    send_broadcast_with_retry(PacketType::TRANSFER_AD,
                              reinterpret_cast<const uint8_t *>(&ad), sizeof(ad));
}

// ── Task 5: Transfer tick — feed fragments into ARQ window ───────────────────

void MeshManager::transfer_tick()
{
    if (!transfer_.active || election_active_) return;
    if (transfer_.exit_node == 0) return;

    while (transfer_.next_fragment < transfer_.fragment_count &&
           !arq_.sender_window_full()) {
        size_t offset = static_cast<size_t>(transfer_.next_fragment) * transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size) ? remain : transfer_.fragment_size;

        int ret = arq_.send_fragment(transfer_.next_fragment,
                                      transfer_.data + offset, frag_len);
        if (ret < 0) break;

        transfer_.next_fragment++;
    }

    // Check if all fragments sent and acknowledged
    if (transfer_.next_fragment >= transfer_.fragment_count &&
        arq_.get_base_seq() >= transfer_.fragment_count) {
        ESP_LOGI(TAG, "File transfer complete: %s", transfer_.filename);
        transfer_.active = false;
        arq_.reset_sender();
    }
}

// ── send_packet / send_raw ───────────────────────────────────────────────────

void MeshManager::send_packet(uint16_t dst, PacketType type,
                               const uint8_t *payload, size_t payload_len)
{
    uint8_t buf[MAX_MTU];
    PacketHeader hdr = {};
    hdr.set_ver_type(PROTOCOL_VERSION, type);
    hdr.src_addr = my_addr_;
    hdr.dst_addr = dst;
    hdr.set_ttl_hops(DEFAULT_TTL, 0);
    hdr.seq_num = 0;

    memcpy(buf, &hdr, PACKET_HEADER_SIZE);
    if (payload && payload_len > 0) {
        memcpy(buf + PACKET_HEADER_SIZE, payload, payload_len);
    }

    size_t total = PACKET_HEADER_SIZE + payload_len;

    if (dst == BROADCAST_ADDR) {
        send_raw(Transport::LORA, buf, total, dst);
    } else {
        NeighborEntry neighbor;
        int8_t rssi = -90;
        uint8_t hops = 0xFF;
        if (route_table_.get_neighbor(dst, neighbor)) {
            rssi = neighbor.rssi;
            hops = neighbor.hop_count;
        }
        Transport t = protocol_selector_.select(rssi, hops, total, 100.0f);
        send_raw(t, buf, total, dst);
    }
}

void MeshManager::send_raw(Transport transport, const uint8_t *data, size_t len,
                            uint16_t peer_addr)
{
    if (transport == Transport::BLE) {
        ble_.send(peer_addr, data, len);
    } else {
        lora_.send(data, len);
    }
}

} // namespace flp
