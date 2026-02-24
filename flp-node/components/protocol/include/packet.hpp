#pragma once

#include <cstddef>
#include <cstdint>

namespace flp
{

static constexpr uint16_t BROADCAST_ADDR = 0xFFFF;
static constexpr uint16_t EXIT_ANY_ADDR = 0xFFFE; // route toward nearest exit
static constexpr uint8_t PROTOCOL_VERSION = 1;
static constexpr uint8_t DEFAULT_TTL = 8;
static constexpr size_t MAX_MTU = 250;
static constexpr uint8_t ARQ_WINDOW = 32;
static constexpr uint32_t ARQ_TIMEOUT = 2000;
static constexpr uint8_t MAX_RETRIES = 3;
static constexpr uint8_t MAX_NEIGHBORS = 16;
static constexpr uint8_t MAX_EXIT_NODES = 4;
static constexpr uint8_t FEC_GROUP_SIZE = 7;

enum class PacketType : uint8_t
{
    DATA = 0x01,
    ACK = 0x02,
    NACK = 0x03,
    DISCOVERY = 0x10,
    ROUTE_REQ = 0x11,
    ROUTE_REPLY = 0x12,
    PARITY = 0x06,
    TRANSFER_AD = 0x20,  // file transfer advertisement
    TRANSFER_ACK = 0x21, // exit node response to transfer ad
    MESH_PUB = 0x30,     // uplink relay: node → exit → MQTT
    MESH_CMD = 0x31,     // downlink relay: MQTT → exit → node
};

// Compact 1-byte relay topic IDs for MESH_PUB payload[0].
// Exit node maps these to full MQTT topic strings: flp/<src_addr>/<suffix>
namespace RelayTopic
{
static constexpr uint8_t HEAP = 0x01;
static constexpr uint8_t METRICS = 0x02;
static constexpr uint8_t TOPOLOGY = 0x03;
static constexpr uint8_t STATUS = 0x04;
} // namespace RelayTopic

// Command IDs for MESH_CMD payload[0].
namespace MeshCmd
{
static constexpr uint8_t REQUEST_TELEMETRY = 0x01;
static constexpr uint8_t CONFIG_UPDATE = 0x02;
static constexpr uint8_t REBOOT = 0x03;
} // namespace MeshCmd

struct __attribute__((packed)) PacketHeader
{
    uint8_t ver_type; // [version:2][type:6]
    uint16_t src_addr;
    uint16_t dst_addr;
    uint8_t ttl_hops; // [ttl:4][hop_count:4]
    uint16_t seq_num;

    // Accessors
    uint8_t version() const
    {
        return ver_type >> 6;
    }
    PacketType type() const
    {
        return static_cast<PacketType>(ver_type & 0x3F);
    }
    uint8_t ttl() const
    {
        return ttl_hops >> 4;
    }
    uint8_t hop_count() const
    {
        return ttl_hops & 0x0F;
    }

    void set_ver_type(uint8_t ver, PacketType t)
    {
        ver_type = (ver << 6) | (static_cast<uint8_t>(t) & 0x3F);
    }
    void set_ttl_hops(uint8_t ttl, uint8_t hops)
    {
        ttl_hops = (ttl << 4) | (hops & 0x0F);
    }
};
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

static constexpr size_t PACKET_HEADER_SIZE = sizeof(PacketHeader);
static constexpr size_t LORA_MAX_PAYLOAD =
    255 - PACKET_HEADER_SIZE;                                       // 247 bytes
static constexpr size_t ESPNOW_MAX_PAYLOAD = 250 - PACKET_HEADER_SIZE; // 242 bytes

struct __attribute__((packed)) DiscoveryPayload
{
    uint8_t flags; // bit 0: has_internet
    uint8_t hops_to_internet;
    int8_t rssi;
    uint8_t wifi_channel; // ESP-NOW channel (0 = unknown)
};

struct __attribute__((packed)) TransferAdPayload
{
    uint32_t session_id;
    uint32_t file_size;
    uint16_t fragment_count;
    uint16_t fragment_size;
    uint32_t crc32;
    char filename[20];
};

struct __attribute__((packed)) TransferAckPayload
{
    uint32_t session_id;
    uint16_t exit_node_addr;
    int8_t rssi_to_gw;
    uint8_t hops_to_gw;
};

// Unified inbound packet used across transports and mesh manager
enum class RxTransport : uint8_t
{
    ESPNOW,
    LORA,
};

struct RxPacket
{
    uint8_t data[MAX_MTU];
    size_t len;
    int8_t rssi;
    RxTransport source;
};

} // namespace flp
