#pragma once

#include <cstdint>
#include <cstddef>

namespace flp {

enum class PacketType : uint8_t {
    DATA        = 0x01,
    ACK         = 0x02,
    NACK        = 0x03,
    DISCOVERY   = 0x10,
    ROUTE_REQ   = 0x11,
    ROUTE_REPLY = 0x12,
    TRANSFER_AD = 0x20, // file transfer advertisement
};

struct __attribute__((packed)) PacketHeader {
    uint8_t    version;
    PacketType type;
    uint16_t   src_addr;
    uint16_t   dst_addr;
    uint8_t    hop_count;
    uint8_t    ttl;
    uint16_t   seq_num;
    uint16_t   payload_len;
};

static constexpr size_t PACKET_HEADER_SIZE = sizeof(PacketHeader);

} // namespace flp
