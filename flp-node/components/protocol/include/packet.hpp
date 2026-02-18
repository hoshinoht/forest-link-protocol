#pragma once

#include <cstdint>
#include <cstddef>

namespace flp {

static constexpr uint16_t BROADCAST_ADDR = 0xFFFF;
static constexpr uint8_t  PROTOCOL_VERSION = 1;
static constexpr uint8_t  DEFAULT_TTL = 8;
static constexpr size_t   MAX_MTU = 512;
static constexpr uint8_t  ARQ_WINDOW = 8;
static constexpr uint32_t ARQ_TIMEOUT = 2000;
static constexpr uint8_t  MAX_RETRIES = 3;
static constexpr uint8_t  MAX_NEIGHBORS = 16;

enum class PacketType : uint8_t {
    DATA         = 0x01,
    ACK          = 0x02,
    NACK         = 0x03,
    DISCOVERY    = 0x10,
    ROUTE_REQ    = 0x11,
    ROUTE_REPLY  = 0x12,
    TRANSFER_AD  = 0x20, // file transfer advertisement
    TRANSFER_ACK = 0x21, // exit node response to transfer ad
};

struct __attribute__((packed)) PacketHeader {
    uint8_t  ver_type;    // [version:2][type:6]
    uint16_t src_addr;
    uint16_t dst_addr;
    uint8_t  ttl_hops;   // [ttl:4][hop_count:4]
    uint16_t seq_num;

    // Accessors
    uint8_t    version()   const { return ver_type >> 6; }
    PacketType type()      const { return static_cast<PacketType>(ver_type & 0x3F); }
    uint8_t    ttl()       const { return ttl_hops >> 4; }
    uint8_t    hop_count() const { return ttl_hops & 0x0F; }

    void set_ver_type(uint8_t ver, PacketType t) {
        ver_type = (ver << 6) | (static_cast<uint8_t>(t) & 0x3F);
    }
    void set_ttl_hops(uint8_t ttl, uint8_t hops) {
        ttl_hops = (ttl << 4) | (hops & 0x0F);
    }
};
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

static constexpr size_t PACKET_HEADER_SIZE = sizeof(PacketHeader);
static constexpr size_t LORA_MAX_PAYLOAD = 255 - PACKET_HEADER_SIZE;  // 247 bytes
static constexpr size_t BLE_MAX_PAYLOAD  = 512 - PACKET_HEADER_SIZE;  // 504 bytes

struct __attribute__((packed)) DiscoveryPayload {
    uint8_t  flags;              // bit 0: has_internet
    uint8_t  hops_to_internet;
    int8_t   rssi;
};

struct __attribute__((packed)) TransferAdPayload {
    uint32_t file_size;
    uint16_t fragment_count;
    uint16_t fragment_size;
    char     filename[20];
};

struct __attribute__((packed)) TransferAckPayload {
    uint16_t exit_node_addr;
    int8_t   rssi_to_gw;
    uint8_t  hops_to_gw;
};

} // namespace flp
