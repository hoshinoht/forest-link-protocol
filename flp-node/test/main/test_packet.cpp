/* test_packet.cpp — Unity tests for PacketHeader bit-field encoding and
 * protocol constants defined in packet.hpp.
 *
 * These are pure struct-manipulation tests with no hardware dependency.
 * All tests must fail until the struct layout and accessor methods in
 * packet.hpp are correct (they already are; tests serve as regression guards).
 */
#include "unity.h"
#include "packet.hpp"

using namespace flp;

/* -------------------------------------------------------------------------
 * setUp / tearDown — no shared state for packet tests
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* Verifies the packed struct is exactly 8 bytes as required by the wire format. */
static void test_packet_header_size_is_8_bytes(void)
{
    TEST_ASSERT_EQUAL(8, sizeof(PacketHeader));
}

/* PACKET_HEADER_SIZE constant must match the actual struct size. */
static void test_packet_header_size_constant_matches_struct(void)
{
    TEST_ASSERT_EQUAL(sizeof(PacketHeader), PACKET_HEADER_SIZE);
}

static void test_broadcast_addr_is_0xFFFF(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, BROADCAST_ADDR);
}

static void test_exit_any_addr_is_0xFFFE(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFE, EXIT_ANY_ADDR);
}

static void test_protocol_version_constant_is_1(void)
{
    TEST_ASSERT_EQUAL_UINT8(1, PROTOCOL_VERSION);
}

static void test_default_ttl_constant_is_8(void)
{
    TEST_ASSERT_EQUAL_UINT8(8, DEFAULT_TTL);
}

static void test_arq_window_constant_is_64(void)
{
    TEST_ASSERT_EQUAL_UINT8(64, ARQ_WINDOW);
}

static void test_fec_group_size_constant_is_7(void)
{
    TEST_ASSERT_EQUAL_UINT8(7, FEC_GROUP_SIZE);
}

/* -------------------------------------------------------------------------
 * ver_type field: version and type encoding / decoding
 * ---------------------------------------------------------------------- */

/* Normal case: version=1 + DATA type. Verifies both nibble regions. */
static void test_set_ver_type_version1_data_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::DATA);
    TEST_ASSERT_EQUAL_UINT8(1, hdr.version());
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::DATA),
                      static_cast<int>(hdr.type()));
}

/* version=1 + DISCOVERY (0x10) — type value occupies lower 6 bits. */
static void test_set_ver_type_version1_discovery_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::DISCOVERY);
    TEST_ASSERT_EQUAL_UINT8(1, hdr.version());
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::DISCOVERY),
                      static_cast<int>(hdr.type()));
}

/* version=3 saturates to 2-bit max (0b11 = 3). Validates that upper 2 bits
 * of ver_type can hold the value 3 without corrupting the type field. */
static void test_set_ver_type_max_version_and_discovery(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(3, PacketType::DISCOVERY);
    TEST_ASSERT_EQUAL_UINT8(3, hdr.version());
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::DISCOVERY),
                      static_cast<int>(hdr.type()));
}

/* PARITY (0x06) must survive an encode/decode round-trip. */
static void test_set_ver_type_parity_type_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::PARITY);
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::PARITY),
                      static_cast<int>(hdr.type()));
}

/* ACK, NACK — verify the full range of common types. */
static void test_set_ver_type_ack_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::ACK);
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::ACK),
                      static_cast<int>(hdr.type()));
}

static void test_set_ver_type_nack_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::NACK);
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::NACK),
                      static_cast<int>(hdr.type()));
}

static void test_set_ver_type_mesh_pub_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::MESH_PUB);
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::MESH_PUB),
                      static_cast<int>(hdr.type()));
}

static void test_set_ver_type_route_error_round_trips(void)
{
    PacketHeader hdr = {};
    hdr.set_ver_type(1, PacketType::ROUTE_ERROR);
    TEST_ASSERT_EQUAL(static_cast<int>(PacketType::ROUTE_ERROR),
                      static_cast<int>(hdr.type()));
}

/* -------------------------------------------------------------------------
 * ttl_hops field: 4-bit TTL and 4-bit hop_count encoding
 * ---------------------------------------------------------------------- */

/* Normal case: ttl=8, hops=3. */
static void test_set_ttl_hops_ttl8_hops3(void)
{
    PacketHeader hdr = {};
    hdr.set_ttl_hops(8, 3);
    TEST_ASSERT_EQUAL_UINT8(8, hdr.ttl());
    TEST_ASSERT_EQUAL_UINT8(3, hdr.hop_count());
}

/* Zero values: both fields should read back as 0. */
static void test_set_ttl_hops_zero_zero(void)
{
    PacketHeader hdr = {};
    hdr.set_ttl_hops(0, 0);
    TEST_ASSERT_EQUAL_UINT8(0, hdr.ttl());
    TEST_ASSERT_EQUAL_UINT8(0, hdr.hop_count());
}

/* Max nibble values (0xF = 15 for both fields). */
static void test_set_ttl_hops_max_nibble_values(void)
{
    PacketHeader hdr = {};
    hdr.set_ttl_hops(15, 15);
    TEST_ASSERT_EQUAL_UINT8(15, hdr.ttl());
    TEST_ASSERT_EQUAL_UINT8(15, hdr.hop_count());
}

/* Overflow: ttl=16 overflows the 4-bit field and wraps to 0.
 * This tests that callers must stay within [0..15] — the encoding
 * is lossy for out-of-range values. */
static void test_set_ttl_hops_ttl_overflow_wraps(void)
{
    PacketHeader hdr = {};
    hdr.set_ttl_hops(16, 0);
    /* 16 << 4 = 0x100; masked to 8 bits = 0x00, so ttl nibble = 0 */
    TEST_ASSERT_EQUAL_UINT8(0, hdr.ttl());
    TEST_ASSERT_EQUAL_UINT8(0, hdr.hop_count());
}

/* -------------------------------------------------------------------------
 * Address fields
 * ---------------------------------------------------------------------- */

/* src and dst address fields must store and retrieve 16-bit values verbatim. */
static void test_packet_header_address_fields_store_correctly(void)
{
    PacketHeader hdr = {};
    hdr.src_addr = 0x1234;
    hdr.dst_addr = 0xFFFF;
    TEST_ASSERT_EQUAL_HEX16(0x1234, hdr.src_addr);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, hdr.dst_addr);
}

/* BROADCAST_ADDR can be stored in dst_addr without truncation. */
static void test_packet_header_dst_can_hold_broadcast_addr(void)
{
    PacketHeader hdr = {};
    hdr.dst_addr = BROADCAST_ADDR;
    TEST_ASSERT_EQUAL_HEX16(BROADCAST_ADDR, hdr.dst_addr);
}

/* Sequence number field stores full 16-bit range. */
static void test_packet_header_seq_num_stores_full_range(void)
{
    PacketHeader hdr = {};
    hdr.seq_num = 0xABCD;
    TEST_ASSERT_EQUAL_HEX16(0xABCD, hdr.seq_num);
}

/* -------------------------------------------------------------------------
 * Payload size constraints
 * ---------------------------------------------------------------------- */

/* LoRa and ESP-NOW max payloads must be smaller than MAX_MTU and positive. */
static void test_lora_max_payload_leaves_room_for_header(void)
{
    TEST_ASSERT_EQUAL(255 - PACKET_HEADER_SIZE, LORA_MAX_PAYLOAD);
    TEST_ASSERT_GREATER_THAN(0, (int)LORA_MAX_PAYLOAD);
}

static void test_espnow_max_payload_leaves_room_for_header(void)
{
    TEST_ASSERT_EQUAL(250 - PACKET_HEADER_SIZE, ESPNOW_MAX_PAYLOAD);
    TEST_ASSERT_GREATER_THAN(0, (int)ESPNOW_MAX_PAYLOAD);
}

/* -------------------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------------- */
void run_packet_tests(void)
{
    RUN_TEST(test_packet_header_size_is_8_bytes);
    RUN_TEST(test_packet_header_size_constant_matches_struct);
    RUN_TEST(test_broadcast_addr_is_0xFFFF);
    RUN_TEST(test_exit_any_addr_is_0xFFFE);
    RUN_TEST(test_protocol_version_constant_is_1);
    RUN_TEST(test_default_ttl_constant_is_8);
    RUN_TEST(test_arq_window_constant_is_64);
    RUN_TEST(test_fec_group_size_constant_is_7);

    RUN_TEST(test_set_ver_type_version1_data_round_trips);
    RUN_TEST(test_set_ver_type_version1_discovery_round_trips);
    RUN_TEST(test_set_ver_type_max_version_and_discovery);
    RUN_TEST(test_set_ver_type_parity_type_round_trips);
    RUN_TEST(test_set_ver_type_ack_round_trips);
    RUN_TEST(test_set_ver_type_nack_round_trips);
    RUN_TEST(test_set_ver_type_mesh_pub_round_trips);
    RUN_TEST(test_set_ver_type_route_error_round_trips);

    RUN_TEST(test_set_ttl_hops_ttl8_hops3);
    RUN_TEST(test_set_ttl_hops_zero_zero);
    RUN_TEST(test_set_ttl_hops_max_nibble_values);
    RUN_TEST(test_set_ttl_hops_ttl_overflow_wraps);

    RUN_TEST(test_packet_header_address_fields_store_correctly);
    RUN_TEST(test_packet_header_dst_can_hold_broadcast_addr);
    RUN_TEST(test_packet_header_seq_num_stores_full_range);

    RUN_TEST(test_lora_max_payload_leaves_room_for_header);
    RUN_TEST(test_espnow_max_payload_leaves_room_for_header);
}
