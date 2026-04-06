/* test_mesh_improvements.cpp — Unity tests for ESP-MESH-inspired improvements.
 *
 * Covers: loop avoidance, load-aware routing, discovery/transfer payload
 * sizes, parent switching hysteresis, congestion bit helpers, and the
 * EXIT_OFFLINE packet struct.
 */
#include "unity.h"
#include "packet.hpp"
#include "route_table.hpp"
#include "esp_timer.h" /* mock — provides g_mock_time_us */

using namespace flp;

static RouteTable g_rt;

void reset_mesh_improvements(void)
{
    g_mock_time_us = 0;
    g_rt = RouteTable();
}

/* =========================================================================
 * Group 1: Loop Avoidance
 * ====================================================================== */

/* Neighbor A routes back to us (inet_origin == my_addr) when routing
 * toward EXIT_ANY_ADDR. Must pick neighbor B instead. */
static void test_next_hop_inet_avoids_self_origin_loop(void)
{
    const uint16_t MY_ADDR = 0x1000;

    /* Neighbor A: 2 hops, routes through us (loop) */
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    g_rt.update_inet_route(0xAAAA, 2, 10, MY_ADDR); /* inet_origin = us */

    /* Neighbor B: 3 hops, routes through real exit */
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 3);
    g_rt.update_inet_route(0xBBBB, 3, 10, 0xEEEE);

    uint16_t hop = g_rt.next_hop(EXIT_ANY_ADDR, MY_ADDR);
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, hop);
}

/* All neighbors route through us — must return BROADCAST_ADDR. */
static void test_next_hop_inet_self_origin_fallback_broadcast(void)
{
    const uint16_t MY_ADDR = 0x1000;

    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    g_rt.update_inet_route(0xAAAA, 2, 10, MY_ADDR);

    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 3);
    g_rt.update_inet_route(0xBBBB, 3, 10, MY_ADDR);

    uint16_t hop = g_rt.next_hop(EXIT_ANY_ADDR, MY_ADDR);
    TEST_ASSERT_EQUAL_HEX16(BROADCAST_ADDR, hop);
}

/* Loop avoidance only applies to EXIT_ANY_ADDR, not unicast. */
static void test_next_hop_unicast_ignores_origin_check(void)
{
    const uint16_t MY_ADDR = 0x1000;

    /* Neighbor is a direct neighbor — unicast should return it regardless
     * of inet_origin. */
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    g_rt.update_inet_route(0xAAAA, 2, 10, MY_ADDR);

    uint16_t hop = g_rt.next_hop(0xAAAA, MY_ADDR);
    TEST_ASSERT_EQUAL_HEX16(0xAAAA, hop); /* direct neighbor, returned as-is */
}

/* =========================================================================
 * Group 2: Load-Aware Cost Routing
 * ====================================================================== */

/* Two neighbors with identical hops + ETX: lower queue_load wins. */
static void test_next_hop_queue_load_breaks_tie(void)
{
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 2);
    /* Both have default etx_x100=100. A has load=20, B has load=5. */
    g_rt.set_queue_load(0xAAAA, 20);
    g_rt.set_queue_load(0xBBBB, 5);

    uint16_t hop = g_rt.next_hop(EXIT_ANY_ADDR);
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, hop); /* lower load wins */
}

/* Hops still dominate — extra hop > lower load. */
static void test_next_hop_queue_load_does_not_override_hops(void)
{
    /* A: 1 hop, load=30 → cost = 100 + 100 + 30 = 230 */
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 1);
    g_rt.set_queue_load(0xAAAA, 30);

    /* B: 2 hops, load=0 → cost = 200 + 100 + 0 = 300 */
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 2);
    g_rt.set_queue_load(0xBBBB, 0);

    uint16_t hop = g_rt.next_hop(EXIT_ANY_ADDR);
    TEST_ASSERT_EQUAL_HEX16(0xAAAA, hop); /* fewer hops wins despite load */
}

/* Serialized entry is now 7 bytes per neighbor. */
static void test_serialize_includes_queue_load(void)
{
    g_rt.update_neighbor(0x1234, -70, 2, true, false);
    g_rt.set_queue_load(0x1234, 42);
    uint8_t buf[64] = {};
    size_t len = g_rt.serialize(buf, sizeof(buf));
    /* 1 header + 1*7 = 8 */
    TEST_ASSERT_EQUAL(8, (int)len);
    TEST_ASSERT_EQUAL_UINT8(1, buf[0]);
    /* queue_load is at offset 1 + 6 = 7 */
    TEST_ASSERT_EQUAL_UINT8(42, buf[7]);
}

/* =========================================================================
 * Group 3: Payload Struct Sizes
 * ====================================================================== */

static void test_discovery_payload_size_is_9(void)
{
    /* Grew from 8 -> 9 bytes when gw_incarnation was appended.
     * Receivers tolerate the legacy 8-byte size; senders always emit 9. */
    TEST_ASSERT_EQUAL(9, (int)sizeof(DiscoveryPayload));
}

static void test_transfer_ack_payload_size_is_7(void)
{
    TEST_ASSERT_EQUAL(7, (int)sizeof(TransferAckPayload));
}

static void test_exit_offline_payload_size_is_4(void)
{
    TEST_ASSERT_EQUAL(4, (int)sizeof(ExitOfflinePayload));
}

/* =========================================================================
 * Group 4: Parent Switching Hysteresis
 * ====================================================================== */

/* Better neighbor seen for only 2 cycles — should NOT switch. */
static void test_better_route_not_accepted_before_threshold(void)
{
    /* Current best: A at cost 300 (2 hops, etx=100) */
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    /* Alternative: B at cost 200 (1 hop, etx=100) — 100 better */
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 1);

    auto r1 = g_rt.check_better_route(0xAAAA);
    TEST_ASSERT_FALSE(r1.should_switch); /* cycle 1 */

    auto r2 = g_rt.check_better_route(0xAAAA);
    TEST_ASSERT_FALSE(r2.should_switch); /* cycle 2 */
}

/* Better neighbor seen for 3 cycles — should switch. */
static void test_better_route_accepted_after_threshold(void)
{
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 1);

    g_rt.check_better_route(0xAAAA); /* 1 */
    g_rt.check_better_route(0xAAAA); /* 2 */
    auto r3 = g_rt.check_better_route(0xAAAA); /* 3 */
    TEST_ASSERT_TRUE(r3.should_switch);
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, r3.new_addr);
}

/* Better neighbor becomes worse mid-tracking — counter resets. */
static void test_better_route_resets_on_regression(void)
{
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 1);

    g_rt.check_better_route(0xAAAA); /* 1 — B is better */
    g_rt.check_better_route(0xAAAA); /* 2 — B is better */

    /* Now B gets worse (increase its hops so cost difference < MIN_COST_IMPROVEMENT) */
    g_rt.set_hops_to_internet(0xBBBB, 2);

    auto r3 = g_rt.check_better_route(0xAAAA); /* reset — no improvement */
    TEST_ASSERT_FALSE(r3.should_switch);

    /* Even after 3 more cycles with B back to better, needs fresh 3 */
    g_rt.set_hops_to_internet(0xBBBB, 1);
    g_rt.check_better_route(0xAAAA); /* 1 */
    g_rt.check_better_route(0xAAAA); /* 2 */
    auto r6 = g_rt.check_better_route(0xAAAA); /* 3 */
    TEST_ASSERT_TRUE(r6.should_switch);
}

/* Marginal improvement (< MIN_COST_IMPROVEMENT) is ignored. */
static void test_better_route_ignores_marginal_improvement(void)
{
    /* A: cost = 200 + 100 = 300, B: cost = 170 + 100 = 270 — diff = 30 < 50 */
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, 2);
    /* B: 1.7 hops? Can't do fractional. Use load instead.
     * A: 2 hops, load=0, etx=100 → cost=300.
     * B: 2 hops, load=0, etx=100 → cost=300. Same — not better.
     * Instead: A at 2 hops (cost=300), B at 2 hops with lower etx via tx success.
     * But etx starts at 100. Let's make A have higher etx. */
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, 2);
    /* Make A cost slightly higher via queue_load: A=300+40=340, B=300+0=300.
     * Diff = 40 < 50 → marginal. */
    g_rt.set_queue_load(0xAAAA, 40);

    g_rt.check_better_route(0xAAAA);
    g_rt.check_better_route(0xAAAA);
    auto r3 = g_rt.check_better_route(0xAAAA);
    TEST_ASSERT_FALSE(r3.should_switch); /* 40 < 50 threshold */
}

/* =========================================================================
 * Group 5: Congestion Bit Helpers
 * ====================================================================== */

static void test_congestion_bit_set_when_high_bit_present(void)
{
    uint16_t raw = 0x8005;
    TEST_ASSERT_TRUE(seq_has_congestion(raw));
    TEST_ASSERT_EQUAL_UINT16(5, seq_strip_congestion(raw));
}

static void test_congestion_bit_clear_when_normal(void)
{
    uint16_t raw = 0x0005;
    TEST_ASSERT_FALSE(seq_has_congestion(raw));
    TEST_ASSERT_EQUAL_UINT16(5, seq_strip_congestion(raw));
}

/* Max fragment count (2200 = 0x0898) — high bit is clear. */
static void test_congestion_bit_max_seq_no_collision(void)
{
    uint16_t raw = 0x0898; /* 2200 */
    TEST_ASSERT_FALSE(seq_has_congestion(raw));
    TEST_ASSERT_EQUAL_UINT16(2200, seq_strip_congestion(raw));

    /* Same seq with congestion flag */
    uint16_t flagged = seq_with_congestion(2200, true);
    TEST_ASSERT_TRUE(seq_has_congestion(flagged));
    TEST_ASSERT_EQUAL_UINT16(2200, seq_strip_congestion(flagged));
}

/* =========================================================================
 * Group 6: EXIT_OFFLINE Payload
 * ====================================================================== */

static void test_exit_offline_payload_fields_pack_correctly(void)
{
    ExitOfflinePayload p = {};
    p.session_id = 0x1234;
    p.exit_node_addr = 0xABCD;

    /* Read back from raw bytes to verify packing */
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&p);
    uint16_t sid, addr;
    memcpy(&sid, raw, 2);
    memcpy(&addr, raw + 2, 2);
    TEST_ASSERT_EQUAL_HEX16(0x1234, sid);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, addr);
}

/* =========================================================================
 * Test runner
 * ====================================================================== */
void run_mesh_improvement_tests(void)
{
    /* Group 1: Loop avoidance */
    RUN_TEST(test_next_hop_inet_avoids_self_origin_loop);
    RUN_TEST(test_next_hop_inet_self_origin_fallback_broadcast);
    RUN_TEST(test_next_hop_unicast_ignores_origin_check);

    /* Group 2: Load-aware routing */
    RUN_TEST(test_next_hop_queue_load_breaks_tie);
    RUN_TEST(test_next_hop_queue_load_does_not_override_hops);
    RUN_TEST(test_serialize_includes_queue_load);

    /* Group 3: Payload sizes */
    RUN_TEST(test_discovery_payload_size_is_9);
    RUN_TEST(test_transfer_ack_payload_size_is_7);
    RUN_TEST(test_exit_offline_payload_size_is_4);

    /* Group 4: Hysteresis */
    RUN_TEST(test_better_route_not_accepted_before_threshold);
    RUN_TEST(test_better_route_accepted_after_threshold);
    RUN_TEST(test_better_route_resets_on_regression);
    RUN_TEST(test_better_route_ignores_marginal_improvement);

    /* Group 5: Congestion bit */
    RUN_TEST(test_congestion_bit_set_when_high_bit_present);
    RUN_TEST(test_congestion_bit_clear_when_normal);
    RUN_TEST(test_congestion_bit_max_seq_no_collision);

    /* Group 6: EXIT_OFFLINE struct */
    RUN_TEST(test_exit_offline_payload_fields_pack_correctly);
}
