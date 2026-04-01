#include "mesh_manager.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.hpp"

using namespace flp;

static const char *TAG = "mesh_mgr";

namespace
{
constexpr uint32_t kArqDedupWindowMs = 200;
constexpr uint32_t kControlDedupWindowMs = 10000;

const char *rx_transport_name(RxTransport source)
{
    switch (source)
    {
        case RxTransport::ESPNOW:
            return "ESP-NOW";
        case RxTransport::LORA:
            return "LoRa";
        default:
            return "UNKNOWN";
    }
}

uint8_t compute_hops_to_internet(const RouteTable &route_table,
                                 bool has_internet)
{
    if (has_internet)
    {
        return 0;
    }

    const uint8_t min_hops = route_table.min_control_hops_to_internet();
    return (min_hops < 0xFE) ? static_cast<uint8_t>(min_hops + 1) : 0xFF;
}
} /* namespace */

void MeshManager::process_slab(BufferSlab *slab)
{
    if (slab->len < PACKET_HEADER_SIZE)
    {
        ESP_LOGW(TAG, "Packet too short: %zu bytes", slab->len);
        return;
    }

    PacketHeader hdr;
    memcpy(&hdr, slab->data, PACKET_HEADER_SIZE);

    /* Drop our own packets (e.g. LoRa broadcast echoes back to sender) */
    if (hdr.src_addr == my_addr_)
    {
        return;
    }

    /*
     * Lab peer blacklist: drop DIRECT packets from blocked addresses.
     * hop_count==0 means we heard this on the radio from the originator.
     * Relayed packets (hop_count>0) are allowed so the mesh still works.
     */
    if (!blocked_peers_.empty() && hdr.hop_count() == 0)
    {
        for (uint16_t bp : blocked_peers_)
        {
            if (hdr.src_addr == bp)
            {
                ESP_LOGD(TAG,
                         "Blacklist: dropping direct pkt from 0x%04X type=0x%02X",
                         hdr.src_addr,
                         static_cast<uint8_t>(hdr.type()));
                return;
            }
        }
    }

    if (hdr.version() != PROTOCOL_VERSION)
    {
        ESP_LOGW(TAG,
                 "Drop non-FLP frame: ver=%u raw_ver_type=0x%02X src=0x%04X "
                 "dst=0x%04X ttl=%u hops=%u len=%zu rssi=%d via=%s "
                 "head=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                 hdr.version(),
                 hdr.ver_type,
                 hdr.src_addr,
                 hdr.dst_addr,
                 hdr.ttl(),
                 hdr.hop_count(),
                 slab->len,
                 slab->rssi,
                 rx_transport_name(slab->source),
                 slab->data[0],
                 slab->data[1],
                 slab->data[2],
                 slab->data[3],
                 slab->data[4],
                 slab->data[5],
                 slab->data[6],
                 slab->data[7]);
        return;
    }

    /*
     * EXIT_OFFLINE must be consumed locally by ALL nodes — sensor nodes need
     * it to redistribute fragments away from the dead exit.  Process BEFORE
     * the TTL=0 gate: a relay that decrements TTL to 0 still needs to handle
     * the packet locally even though it won't forward further.
     */
    const uint8_t *payload = slab->data + PACKET_HEADER_SIZE;
    size_t payload_len = slab->len - PACKET_HEADER_SIZE;

    if (hdr.type() == PacketType::EXIT_OFFLINE &&
        payload_len >= sizeof(ExitOfflinePayload))
    {
        ExitOfflinePayload eop;
        memcpy(&eop, payload, sizeof(eop));
        transfer_engine_.handle_exit_offline(
            eop.exit_node_addr, eop.session_id);
    }

    if (hdr.ttl() == 0)
    {
        ESP_LOGD(TAG, "Dropping packet, TTL=0");
        return;
    }

    /*
     * Update route table only for direct packets (hop_count == 0).
     * Forwarded packets (hop_count > 0) carry the ORIGINAL source address,
     * not the relay that sent them. Recording them as neighbors creates
     * unreachable entries that cause next_hop() to return addresses outside
     * radio range, silently losing packets.
     */
    if (hdr.hop_count() == 0)
    {
        bool via_espnow = (slab->source == RxTransport::ESPNOW);
        bool via_lora = (slab->source == RxTransport::LORA);
        route_table_.update_neighbor(
            hdr.src_addr, slab->rssi, hdr.hop_count(), via_espnow, via_lora);
    }

    /* EXIT_ANY_ADDR: consumed by exit nodes (has MQTT), relayed by others.
     * Require MQTT to be actually connected — has_internet_ only means
     * "WiFi has IP", but DNS resolution + TLS handshake can take 10-15s.
     * Accepting exit role before MQTT connects causes the fragment queue
     * to fill with no drain path, producing zero ACKs and false timeouts. */
    bool mqtt_connected = mqtt_client_ && mqtt_client_->is_connected();
    bool is_exit = has_internet_ && mqtt_connected;
    bool for_us = (hdr.dst_addr == my_addr_) ||
                  (hdr.dst_addr == BROADCAST_ADDR) ||
                  (hdr.dst_addr == EXIT_ANY_ADDR && is_exit);

    if (for_us)
    {
        switch (hdr.type())
        {
            case PacketType::DISCOVERY:
                handle_discovery(hdr, slab->source, payload, payload_len);
                break;
            case PacketType::TRANSFER_AD:
                transfer_engine_.handle_transfer_ad(
                    hdr, payload, payload_len, is_exit);
                break;
            case PacketType::TRANSFER_ACK:
                transfer_engine_.handle_transfer_ack(hdr, payload, payload_len);
                break;
            case PacketType::DATA:
            case PacketType::PARITY:
                transfer_engine_.handle_data(hdr, payload, payload_len);
                break;
            case PacketType::ACK:
            {
                bool cong = seq_has_congestion(hdr.seq_num);
                uint16_t seq = seq_strip_congestion(hdr.seq_num);
                transfer_engine_.handle_ack(seq, hdr.src_addr);
                if (cong)
                {
                    transfer_engine_.signal_congestion();
                }
                break;
            }
            case PacketType::NACK:
            {
                bool cong = seq_has_congestion(hdr.seq_num);
                uint16_t seq = seq_strip_congestion(hdr.seq_num);
                transfer_engine_.handle_nack(seq, hdr.src_addr);
                if (cong)
                {
                    transfer_engine_.signal_congestion();
                }
                break;
            }
            case PacketType::MESH_PUB:
                handle_mesh_pub(hdr, payload, payload_len);
                break;
            case PacketType::MESH_CMD:
                handle_mesh_cmd(hdr, payload, payload_len);
                break;
            case PacketType::ROUTE_ERROR:
                handle_route_error(hdr, payload, payload_len);
                break;
            case PacketType::TRANSFER_DONE:
                transfer_engine_.handle_transfer_done(hdr.src_addr,
                                                     payload,
                                                     payload_len);
                break;
            default:
                ESP_LOGD(TAG,
                         "Unhandled packet type 0x%02X",
                         static_cast<uint8_t>(hdr.type()));
                break;
        }
    }

    /*
     * Forward if not for us (not unicast-to-us, not broadcast).
     * EXIT_ANY_ADDR packets are forwarded by non-exit nodes — next_hop()
     * won't find 0xFFFE in neighbors so it falls back to routing toward
     * the lowest hops_to_internet, which is exactly what we want.
     */
    bool should_forward = (hdr.dst_addr != my_addr_) &&
                          (hdr.dst_addr != BROADCAST_ADDR) &&
                          !(hdr.dst_addr == EXIT_ANY_ADDR && is_exit);
    if (should_forward)
    {
        /*
         * Fix 4: Dedup — drop packets we've already forwarded to prevent
         * broadcast storm (O(TTL × relays) amplification per packet).
         *
         * ARQ-controlled types (DATA/PARITY/ACK/NACK) use a short 200ms
         * window so legitimate retransmits (after 700ms+ ARQ timeout) pass
         * through, while relay echo loops (~50ms) are still blocked.
         * Control packets keep the 10s window for broadcast storm prevention.
         */
        PacketType ptype = hdr.type();
        bool arq_type = (ptype == PacketType::DATA ||
                         ptype == PacketType::PARITY ||
                         ptype == PacketType::ACK ||
                         ptype == PacketType::NACK);
        uint32_t dedup_window = arq_type ? kArqDedupWindowMs
                                         : kControlDedupWindowMs;

        if (already_seen(hdr.src_addr,
                         hdr.dst_addr,
                         static_cast<uint8_t>(hdr.type()),
                         hdr.seq_num,
                         dedup_window))
        {
            ESP_LOGD(TAG,
                     "Dedup: dropping already-seen packet from 0x%04X seq=%u",
                     hdr.src_addr,
                     hdr.seq_num);
            return;
        }
        forward_packet(slab, hdr);
    }
}

void MeshManager::handle_discovery(const PacketHeader &hdr,
                                   RxTransport source,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    if (payload_len < sizeof(DiscoveryPayload))
    {
        return;
    }

    DiscoveryPayload disc;
    memcpy(&disc, payload, sizeof(disc));

    const char *rx_transport = rx_transport_name(source);

    /* Extract wifi channel from flags bits 1-4 */
    uint8_t disc_wifi_ch = (disc.flags >> kDiscoveryChShift) & kDiscoveryChMask;

    ESP_LOGI(TAG,
             "Discovery from 0x%04X via %s: inet=%u hops_inet=%u rssi=%d "
             "ch=%u seq=%u origin=0x%04X",
             hdr.src_addr,
             rx_transport,
             (disc.flags & kDiscoveryInternetFlag) != 0U,
             disc.hops_to_internet,
             disc.rssi,
             disc_wifi_ch,
             disc.inet_seq,
             disc.inet_origin);

    if ((disc.flags & kDiscoveryInternetFlag) != 0U)
    {
        route_table_.set_has_internet(hdr.src_addr, true);
    }

    /*
     * Auto-sync ESP-NOW channel: if the peer advertises a valid channel
     * and we're not connected to an AP (relay node or disconnected exit),
     * switch to match so ESP-NOW can reach the mesh.
     */
    if (disc_wifi_ch > 0 && !has_internet_)
    {
        /*
         * Fix 2: Only call esp_wifi_set_channel() when the channel actually
         * differs from what we last set. Redundant calls during active
         * ESP-NOW operation can corrupt peer channel state.
         * current_channel_ is initialised to 0; query the driver on first use.
         */
        if (current_channel_ == 0)
        {
            wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&current_channel_, &sec);
        }
        if (current_channel_ != disc_wifi_ch)
        {
            esp_wifi_set_channel(disc_wifi_ch, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG,
                     "ESP-NOW channel synced: %u -> %u",
                     current_channel_,
                     disc_wifi_ch);
            current_channel_ = disc_wifi_ch;
        }
    }

    /*
     * Step 1e: Use DSDV sequence-numbered update instead of plain
     * set_hops_to_internet. Only accept if sequence check passes.
     */
    route_table_.update_inet_route(
        hdr.src_addr, disc.hops_to_internet, disc.inet_seq, disc.inet_origin);
    route_table_.set_queue_load(hdr.src_addr, disc.queue_load);

    /*
     * Respond to discovery requests only. Unicast discovery packets are already
     * responses, so replying again causes a ping-pong storm.
     */
    if (hdr.dst_addr == BROADCAST_ADDR)
    {
        DiscoveryPayload resp = {};
        resp.flags = has_internet_ ? kDiscoveryInternetFlag : 0x00;
        resp.hops_to_internet =
            compute_hops_to_internet(route_table_, has_internet_);
        resp.rssi = 0;

        /* Pack WiFi channel into flags bits 1-4 */
        uint8_t resp_ch = 0;
        wifi_second_chan_t resp_sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&resp_ch, &resp_sec);
        resp.flags |= (resp_ch & kDiscoveryChMask) << kDiscoveryChShift;

        /* Step 1d: Populate sequence info */
        if (has_internet_)
        {
            resp.inet_seq = my_inet_seq_;
            resp.inet_origin = my_addr_;
        }
        else
        {
            auto best = route_table_.best_inet_route();
            resp.inet_seq = best.seq;
            resp.inet_origin = best.origin;
        }

        /* Populate queue load for load-aware routing (same as broadcast) */
        resp.queue_load = saturated_queue_load();

        /* Step 7c: Encode current SF in flags bits 5-7 */
        uint8_t sf_enc =
            static_cast<uint8_t>(lora_.get_spreading_factor() - 5) & 0x07;
        resp.flags |= (sf_enc << kDiscoverySfShift);

        /*
         * Reply via the same transport the request arrived on.
         * If a relay on a different WiFi channel sent discovery via LoRa,
         * responding via ESP-NOW would silently fail (channel mismatch).
         */
        if (source == RxTransport::LORA)
        {
            uint8_t *resp_buf = scratch_buf_;
            PacketHeader resp_hdr = {};
            resp_hdr.set_ver_type(PROTOCOL_VERSION, PacketType::DISCOVERY);
            resp_hdr.src_addr = my_addr_;
            resp_hdr.dst_addr = hdr.src_addr;
            resp_hdr.set_ttl_hops(DEFAULT_TTL, 0);
            resp_hdr.seq_num = 0;
            memcpy(resp_buf, &resp_hdr, PACKET_HEADER_SIZE);
            memcpy(resp_buf + PACKET_HEADER_SIZE, &resp, sizeof(resp));
            send_raw(Transport::LORA,
                     resp_buf,
                     PACKET_HEADER_SIZE + sizeof(resp),
                     hdr.src_addr);
        }
        else
        {
            send_packet(hdr.src_addr,
                        PacketType::DISCOVERY,
                        reinterpret_cast<const uint8_t *>(&resp),
                        sizeof(resp));
        }
    }
}

void MeshManager::send_route_error(uint16_t dead_addr, uint16_t inet_origin,
                                   uint16_t last_seq)
{
    RouteErrorPayload rerr = {};
    rerr.dead_addr = dead_addr;
    rerr.inet_origin = inet_origin;
    rerr.last_known_seq = last_seq;

    ESP_LOGI(TAG, "Sending ROUTE_ERROR: dead=0x%04X origin=0x%04X seq=%u",
             dead_addr, inet_origin, last_seq);

    send_packet(BROADCAST_ADDR,
                PacketType::ROUTE_ERROR,
                reinterpret_cast<const uint8_t *>(&rerr),
                sizeof(rerr));
}

void MeshManager::handle_route_error(const PacketHeader &hdr,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    if (payload_len < sizeof(RouteErrorPayload))
    {
        return;
    }

    RouteErrorPayload rerr;
    memcpy(&rerr, payload, sizeof(rerr));

    ESP_LOGI(TAG, "ROUTE_ERROR from 0x%04X: dead=0x%04X origin=0x%04X",
             hdr.src_addr, rerr.dead_addr, rerr.inet_origin);

    /* Check if our best route uses the dead address */
    if (route_table_.invalidate_route_via(rerr.dead_addr))
    {
        /* We depend on this path — rebroadcast the error */
        send_route_error(rerr.dead_addr, rerr.inet_origin, rerr.last_known_seq);
    }
}

void MeshManager::forward_packet(BufferSlab *slab, const PacketHeader &hdr)
{
    /* Step 5b: Congestion-aware forwarding */
    UBaseType_t hi_spaces = uxQueueSpacesAvailable(hi_pri_queue_);
    UBaseType_t lo_spaces = uxQueueSpacesAvailable(lo_pri_queue_);
    bool congested = (hi_spaces + lo_spaces) < (kQueueDepth / 2);

    if (congested)
    {
        PacketType t = hdr.type();
        bool high_priority = (t == PacketType::ACK || t == PacketType::NACK ||
                              t == PacketType::ROUTE_ERROR ||
                              t == PacketType::TRANSFER_ACK ||
                              t == PacketType::TRANSFER_AD ||
                              t == PacketType::EXIT_OFFLINE);
        if (!high_priority)
        {
            /* Relay congestion echo: immediately NACK dropped DATA/PARITY
             * fragments so the sender's ARQ retransmits without waiting
             * for the full timeout.  The congestion flag triggers
             * signal_congestion() on the sender, throttling it. */
            if (t == PacketType::DATA || t == PacketType::PARITY)
            {
                uint16_t nack_seq = seq_with_congestion(hdr.seq_num, true);
                send_packet(hdr.src_addr, PacketType::NACK, nullptr, 0, nack_seq);
            }
            ESP_LOGD(TAG, "Congestion drop: type=0x%02X from 0x%04X seq=%u",
                     static_cast<uint8_t>(t), hdr.src_addr, hdr.seq_num);
            return;
        }
    }

    /* Mutate in-place: decrement TTL, increment hop count */
    PacketHeader *fwd_hdr = reinterpret_cast<PacketHeader *>(slab->data);
    fwd_hdr->set_ttl_hops(fwd_hdr->ttl() - 1, fwd_hdr->hop_count() + 1);

    /*
     * P4 fix: Broadcast TRANSFER_AD so ALL exit nodes in range hear it.
     * Normal unicast forwarding only reaches one exit, breaking multi-exit
     * election when the sensor is behind relay node(s).
     */
    if (hdr.type() == PacketType::TRANSFER_AD ||
        hdr.type() == PacketType::EXIT_OFFLINE)
    {
        ESP_LOGD(TAG, "Broadcasting %s from 0x%04X, ttl=%u",
                 hdr.type() == PacketType::TRANSFER_AD ? "TRANSFER_AD"
                                                       : "EXIT_OFFLINE",
                 hdr.src_addr, fwd_hdr->ttl());
        send_raw(Transport::ESPNOW, slab->data, slab->len, BROADCAST_ADDR);
        send_raw(Transport::LORA, slab->data, slab->len, BROADCAST_ADDR);
        return;
    }

    uint16_t next = route_table_.next_hop(hdr.dst_addr, my_addr_);
    if (next == BROADCAST_ADDR)
    {
        ESP_LOGW(TAG, "No route to 0x%04X, dropping", hdr.dst_addr);
        return;
    }

    /* Piggyback congestion signal on forwarded ACK/NACK packets.
     * Set the high bit of hdr.seq_num (bit 15). Safe because the max
     * fragment count is 8192 (0x2000) — bit 15 is always clear. */
    if ((hdr.type() == PacketType::ACK || hdr.type() == PacketType::NACK) &&
        congested)
    {
        fwd_hdr->seq_num |= CONGESTION_FLAG;
    }

    NeighborEntry neighbor;
    int8_t rssi = kDefaultRssi;
    if (route_table_.get_neighbor(next, neighbor))
    {
        rssi = neighbor.rssi;
    }

    Transport t = requires_espnow_data_path(hdr.type())
                      ? Transport::ESPNOW
                      : protocol_selector_.select(rssi,
                                                  fwd_hdr->hop_count(),
                                                  slab->len,
                                                  kLinkQualityPct);

    ESP_LOGD(TAG,
             "Forwarding to 0x%04X via 0x%04X (%s), ttl=%u",
             hdr.dst_addr,
             next,
             (t == Transport::ESPNOW) ? "ESPNOW" : "LoRa",
             fwd_hdr->ttl());

    send_raw(t, slab->data, slab->len, next);
}

uint8_t MeshManager::saturated_queue_load() const
{
    UBaseType_t hi_used = kQueueDepth - uxQueueSpacesAvailable(hi_pri_queue_);
    UBaseType_t lo_used = kQueueDepth - uxQueueSpacesAvailable(lo_pri_queue_);
    uint32_t total = hi_used + lo_used;
    return (total > 255) ? 255 : static_cast<uint8_t>(total);
}

void MeshManager::send_discovery()
{
    DiscoveryPayload disc = {};
    disc.flags = has_internet_ ? kDiscoveryInternetFlag : 0x00;
    disc.hops_to_internet =
        compute_hops_to_internet(route_table_, has_internet_);
    disc.rssi = 0;

    /* Pack WiFi channel into flags bits 1-4 */
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &sec);
    disc.flags |= (ch & kDiscoveryChMask) << kDiscoveryChShift;

    /* Step 1d: Populate DSDV sequence info */
    if (has_internet_)
    {
        disc.inet_seq = ++my_inet_seq_;
        disc.inet_origin = my_addr_;
    }
    else
    {
        auto best = route_table_.best_inet_route();
        disc.inet_seq = best.seq;
        disc.inet_origin = best.origin;
    }

    /* Populate queue load for load-aware routing */
    disc.queue_load = saturated_queue_load();

    /* Step 7c: Encode current SF in flags bits 5-7 */
    uint8_t sf_enc =
        static_cast<uint8_t>(lora_.get_spreading_factor() - 5) & 0x07;
    disc.flags |= (sf_enc << kDiscoverySfShift);

    /* Build raw packet for direct transport control */
    uint8_t *buf = scratch_buf_;
    PacketHeader hdr = {};
    hdr.set_ver_type(PROTOCOL_VERSION, PacketType::DISCOVERY);
    hdr.src_addr = my_addr_;
    hdr.dst_addr = BROADCAST_ADDR;
    hdr.set_ttl_hops(DEFAULT_TTL, 0);
    hdr.seq_num = 0;
    memcpy(buf, &hdr, PACKET_HEADER_SIZE);
    memcpy(buf + PACKET_HEADER_SIZE, &disc, sizeof(disc));
    size_t total = PACKET_HEADER_SIZE + sizeof(disc);

    /*
     * Step 5a: Smart discovery transport selection.
     * Always broadcast on ESP-NOW (fast, low cost).
     * Only broadcast on LoRa if we have no ESP-NOW neighbors
     * OR if we haven't heard from any neighbor in 20s (bootstrap).
     */
    send_raw(Transport::ESPNOW, buf, total, BROADCAST_ADDR);

    /*
     * Send LoRa discovery when:
     *  - no ESP-NOW neighbors at all (bootstrap), OR
     *  - no neighbor contact in 20s (stale mesh), OR
     *  - no route to internet yet (gateway may be on a different WiFi
     *    channel, unreachable via ESP-NOW but reachable via LoRa)
     */
    bool lora_tx = lora_.is_initialized() &&
                   (route_table_.get_espnow_neighbor_count() == 0 ||
                    route_table_.oldest_contact_age_ms() > 20000 ||
                    disc.hops_to_internet >= ROUTE_HOPS_UNKNOWN);
    if (lora_tx)
    {
        send_raw(Transport::LORA, buf, total, BROADCAST_ADDR);
    }

    ESP_LOGI(TAG,
             "Discovery TX: ch=%u hops_inet=%u espnow_nbrs=%u lora=%s",
             ch,
             disc.hops_to_internet,
             (unsigned) route_table_.get_espnow_neighbor_count(),
             lora_tx ? "yes" : "no");

    /* Hysteresis: track preferred parent and only switch after sustained
     * improvement across SWITCH_THRESHOLD_CYCLES (default 3) discovery
     * cycles, preventing route flapping in marginal link conditions. */
    if (!has_internet_)
    {
        uint16_t current = (preferred_parent_ != BROADCAST_ADDR)
                               ? preferred_parent_
                               : route_table_.next_hop(EXIT_ANY_ADDR, my_addr_);
        auto result = route_table_.check_better_route(current, my_addr_);
        if (result.should_switch)
        {
            ESP_LOGI(TAG,
                     "Hysteresis: switching preferred parent 0x%04X -> 0x%04X",
                     preferred_parent_, result.new_addr);
            preferred_parent_ = result.new_addr;
        }
        else if (preferred_parent_ == BROADCAST_ADDR && current != BROADCAST_ADDR)
        {
            /* First time: adopt the current best without hysteresis delay */
            preferred_parent_ = current;
        }
    }
}
