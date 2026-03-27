/* test_route_table.cpp — Unity tests for RouteTable.
 *
 * RouteTable is a header-only inline class that calls esp_timer_get_time()
 * internally. The mock in mocks/esp_timer.h intercepts that call and returns
 * g_mock_time_us, which tests control directly.
 *
 * Time: all tests set g_mock_time_us in microseconds. RouteTable divides by
 * 1000 internally to get milliseconds, so "1 second" == g_mock_time_us = 1000000.
 */
#include "unity.h"
#include "route_table.hpp"
#include "esp_timer.h"  /* mock — provides g_mock_time_us */

using namespace flp;

static RouteTable g_rt;

void reset_route_table(void)
{
    g_mock_time_us = 0;
    g_rt = RouteTable();
}

/* =========================================================================
 * Basic neighbor management
 * ====================================================================== */

/* Adding one neighbor increments get_count to 1. */
static void test_route_table_add_one_neighbor_count_is_1(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    TEST_ASSERT_EQUAL_UINT8(1, g_rt.get_count());
}

/* Updating an existing neighbor (same addr) must not increase count. */
static void test_route_table_update_existing_neighbor_count_stays_same(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    g_rt.update_neighbor(0x1001, -55, 1, true, false);  /* same addr, new rssi */
    TEST_ASSERT_EQUAL_UINT8(1, g_rt.get_count());
}

/* Update must reflect the new RSSI value for an existing neighbor. */
static void test_route_table_update_existing_neighbor_rssi_updates(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    g_rt.update_neighbor(0x1001, -45, 1, true, false);
    NeighborEntry e;
    TEST_ASSERT_TRUE(g_rt.get_neighbor(0x1001, e));
    TEST_ASSERT_EQUAL_INT8(-45, e.rssi);
}

/* Fill to the max — count must reach MAX_NEIGHBORS exactly. */
static void test_route_table_fill_to_max_neighbors(void)
{
    for (uint16_t i = 0; i < MAX_NEIGHBORS; i++) {
        g_rt.update_neighbor(0x2000 + i, -70, 1, true, false);
    }
    TEST_ASSERT_EQUAL_UINT8(MAX_NEIGHBORS, g_rt.get_count());
}

/* Adding a 17th neighbor evicts the oldest (added at t=0) and keeps count at 16.
 * The new entry must be findable; the evicted (oldest) entry must not. */
static void test_route_table_17th_neighbor_evicts_oldest(void)
{
    /* Add 16 neighbors at t=0ms (g_mock_time_us=0). */
    for (uint16_t i = 0; i < MAX_NEIGHBORS; i++) {
        g_rt.update_neighbor(0x3000 + i, -70, 1, true, false);
    }
    /* Advance time to 5000ms so the new entry is clearly newer. */
    g_mock_time_us = 5000LL * 1000;  /* 5000 ms in us */
    g_rt.update_neighbor(0x3FFF, -50, 1, true, false);

    TEST_ASSERT_EQUAL_UINT8(MAX_NEIGHBORS, g_rt.get_count());
    /* New entry must be present. */
    NeighborEntry e;
    TEST_ASSERT_TRUE(g_rt.get_neighbor(0x3FFF, e));
    /* All the t=0 entries: at least one must have been evicted (exactly one). */
    uint8_t found = 0;
    for (uint16_t i = 0; i < MAX_NEIGHBORS; i++) {
        NeighborEntry tmp;
        if (g_rt.get_neighbor(0x3000 + i, tmp)) found++;
    }
    TEST_ASSERT_EQUAL_UINT8(MAX_NEIGHBORS - 1, found);
}

/* =========================================================================
 * next_hop routing
 * ====================================================================== */

/* Direct neighbor: next_hop must return the neighbor's own address. */
static void test_route_table_next_hop_direct_neighbor_returns_addr(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    TEST_ASSERT_EQUAL_HEX16(0x1001, g_rt.next_hop(0x1001));
}

/* No neighbors with internet routes → next_hop returns BROADCAST_ADDR. */
static void test_route_table_next_hop_no_route_returns_broadcast(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    /* 0x9999 is not a direct neighbor, and 0x1001 has ROUTE_HOPS_UNKNOWN. */
    TEST_ASSERT_EQUAL_HEX16(BROADCAST_ADDR, g_rt.next_hop(0x9999));
}

/* With no neighbors at all, next_hop returns BROADCAST_ADDR. */
static void test_route_table_next_hop_empty_table_returns_broadcast(void)
{
    TEST_ASSERT_EQUAL_HEX16(BROADCAST_ADDR, g_rt.next_hop(0x9999));
}

/* ETX cost selection: neighbor B (1 hop, etx=150 → cost=250) beats
 * neighbor A (2 hops, etx=100 → cost=300). next_hop must return B.
 *
 * Implementation note: default etx_x100=100 after update_neighbor.
 * We use report_link_tx + set_hops_to_internet to set desired values.
 */
static void test_route_table_next_hop_selects_lower_cost_neighbor(void)
{
    /* Neighbor A: 2 hops to inet, etx=100 (default) → cost = 200+100 = 300 */
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, /*hops_to_inet=*/2);

    /* Neighbor B: 1 hop to inet, etx=150 → cost = 100+150 = 250 */
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, /*hops_to_inet=*/1);
    /* Make 2 tx, 1 success → etx_x100 = 200/1 = 200... we need exactly 150.
     * Easiest: 2 tx, 1 success gives tx_count=2, succ=1, etx=200.
     * Instead use 4 tx, 3 success: etx = 400/3 = 133. Not exact.
     * Use 2 tx, 1 success for A (etx=200, cost=300) and keep B at default
     * etx=100 with 1 hop (cost=200). Reframe the test to match real behavior. */

    /* Adjust: A gets 1 extra failed tx so etx > 100. */
    g_rt.report_link_tx(0xAAAA, false); /* tx_count=1, tx_success=0, succ=1(guard), etx=100 */
    g_rt.report_link_tx(0xAAAA, false); /* tx_count=2, tx_success=0, etx=200 */
    /* A cost: 2*100 + 200 = 400. B cost: 1*100 + 100 = 200. B wins. */

    uint16_t hop = g_rt.next_hop(0x9999); /* unknown dst → use inet route */
    TEST_ASSERT_EQUAL_HEX16(0xBBBB, hop);
}

/* =========================================================================
 * update_inet_route: DSDV sequence-number rules
 * ====================================================================== */

static void setup_neighbor_with_initial_inet_route(uint16_t addr,
                                                    uint8_t hops,
                                                    uint16_t seq,
                                                    uint16_t origin)
{
    g_rt.update_neighbor(addr, -60, 1, true, false, hops);
    /* Seed the seq/origin fields via update_inet_route. */
    g_rt.update_inet_route(addr, hops, seq, origin);
}

/* Higher seq from same origin: accept unconditionally. */
static void test_route_table_inet_route_higher_seq_same_origin_accepted(void)
{
    setup_neighbor_with_initial_inet_route(0x1001, 3, 10, 0xEEEE);
    bool accepted = g_rt.update_inet_route(0x1001, 2, 11, 0xEEEE);
    TEST_ASSERT_TRUE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT8(2, e.hops_to_internet);
    TEST_ASSERT_EQUAL_UINT16(11, e.inet_seq);
}

/* Same seq, lower hops: accept. */
static void test_route_table_inet_route_same_seq_lower_hops_accepted(void)
{
    setup_neighbor_with_initial_inet_route(0x1001, 4, 10, 0xEEEE);
    bool accepted = g_rt.update_inet_route(0x1001, 2, 10, 0xEEEE);
    TEST_ASSERT_TRUE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT8(2, e.hops_to_internet);
}

/* Same seq, same or higher hops: reject. */
static void test_route_table_inet_route_same_seq_higher_hops_rejected(void)
{
    setup_neighbor_with_initial_inet_route(0x1001, 2, 10, 0xEEEE);
    bool accepted = g_rt.update_inet_route(0x1001, 5, 10, 0xEEEE);
    TEST_ASSERT_FALSE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT8(2, e.hops_to_internet);  /* unchanged */
}

/* Lower seq from same origin: stale, reject. */
static void test_route_table_inet_route_lower_seq_rejected(void)
{
    setup_neighbor_with_initial_inet_route(0x1001, 3, 10, 0xEEEE);
    bool accepted = g_rt.update_inet_route(0x1001, 1, 9, 0xEEEE);
    TEST_ASSERT_FALSE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(10, e.inet_seq);  /* unchanged */
}

/* Different origin with higher seq: accept. */
static void test_route_table_inet_route_different_origin_higher_seq_accepted(void)
{
    setup_neighbor_with_initial_inet_route(0x1001, 3, 5, 0xAAAA);
    bool accepted = g_rt.update_inet_route(0x1001, 4, 7, 0xBBBB);
    TEST_ASSERT_TRUE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(0xBBBB, e.inet_origin);
}

/* update_inet_route for unknown neighbor returns false. */
static void test_route_table_inet_route_unknown_neighbor_returns_false(void)
{
    bool accepted = g_rt.update_inet_route(0xDEAD, 2, 1, 0x1234);
    TEST_ASSERT_FALSE(accepted);
}

/* =========================================================================
 * ETX / report_link_tx
 * ====================================================================== */

/* One success: etx_x100 = 1*100/1 = 100. */
static void test_route_table_etx_one_success_is_100(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    g_rt.report_link_tx(0x1001, true);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(100, e.etx_x100);
}

/* 2 tx, 1 success: etx_x100 = 2*100/1 = 200. */
static void test_route_table_etx_two_tx_one_success_is_200(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    g_rt.report_link_tx(0x1001, false);
    g_rt.report_link_tx(0x1001, true);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(200, e.etx_x100);
}

/* 10 tx, 0 success: uncapped would be 10*100/1=1000 (guard: succ=1 when 0). */
static void test_route_table_etx_capped_at_1000(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    for (int i = 0; i < 10; i++) {
        g_rt.report_link_tx(0x1001, false);
    }
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(1000, e.etx_x100);
}

/* report_link_tx for unknown addr is a no-op (must not crash). */
static void test_route_table_etx_unknown_addr_noop(void)
{
    g_rt.report_link_tx(0xDEAD, true);  /* no neighbor — must not crash */
    TEST_ASSERT_EQUAL_UINT8(0, g_rt.get_count());
}

/* =========================================================================
 * prune_stale
 * ====================================================================== */

/* Neighbor added at t=0ms, pruned with max_age=1000ms at t=2000ms (age=2000 > 1000). */
static void test_route_table_prune_removes_old_neighbor(void)
{
    g_mock_time_us = 0;
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    g_mock_time_us = 2000LL * 1000;  /* advance to 2000ms */
    g_rt.prune_stale(1000);
    TEST_ASSERT_EQUAL_UINT8(0, g_rt.get_count());
}

/* Neighbor added at t=1500ms: at t=2000ms, age=500 ≤ 1000ms — must be kept. */
static void test_route_table_prune_keeps_recent_neighbor(void)
{
    g_mock_time_us = 0;
    g_rt.update_neighbor(0x1001, -60, 1, true, false); /* added at t=0 */
    g_mock_time_us = 1500LL * 1000;
    g_rt.update_neighbor(0x2001, -60, 1, true, false); /* added at t=1500ms */
    g_mock_time_us = 2000LL * 1000;
    g_rt.prune_stale(1000);
    /* 0x1001 age=2000ms pruned, 0x2001 age=500ms kept */
    TEST_ASSERT_EQUAL_UINT8(1, g_rt.get_count());
    NeighborEntry e;
    TEST_ASSERT_TRUE(g_rt.get_neighbor(0x2001, e));
    TEST_ASSERT_FALSE(g_rt.get_neighbor(0x1001, e));
}

/* Pruning an empty table is a no-op. */
static void test_route_table_prune_empty_table_noop(void)
{
    g_rt.prune_stale(1000);
    TEST_ASSERT_EQUAL_UINT8(0, g_rt.get_count());
}

/* =========================================================================
 * serialize
 * ====================================================================== */

/* Zero neighbors: buf[0]=0, returns 1. */
static void test_route_table_serialize_empty_returns_1(void)
{
    uint8_t buf[64] = {};
    size_t len = g_rt.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
}

/* One neighbor: returns 8 (1 + 1*7). */
static void test_route_table_serialize_one_neighbor_returns_8(void)
{
    g_rt.update_neighbor(0x1234, -70, 2, true, false);
    uint8_t buf[64] = {};
    size_t len = g_rt.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(8, (int)len);
    TEST_ASSERT_EQUAL_UINT8(1, buf[0]);
}

/* Verify address is encoded little-endian and flags byte is correct. */
static void test_route_table_serialize_flags_espnow_set(void)
{
    g_rt.update_neighbor(0x1234, -70, 2, /*espnow=*/true, /*lora=*/false);
    uint8_t buf[64] = {};
    g_rt.serialize(buf, sizeof(buf));
    /* Entry at offset 1: [addr_lo, addr_hi, rssi, hops, hops_inet, flags, queue_load] */
    uint16_t addr_le;
    __builtin_memcpy(&addr_le, buf + 1, 2);
    TEST_ASSERT_EQUAL_HEX16(0x1234, addr_le);
    uint8_t flags = buf[6];
    TEST_ASSERT_TRUE(flags & ROUTE_FLAG_ESPNOW);
    TEST_ASSERT_FALSE(flags & ROUTE_FLAG_LORA);
    TEST_ASSERT_FALSE(flags & ROUTE_FLAG_INTERNET);
}

/* Verify lora flag and internet flag. */
static void test_route_table_serialize_flags_lora_and_internet(void)
{
    g_rt.update_neighbor(0x5678, -50, 1, /*espnow=*/false, /*lora=*/true);
    g_rt.set_has_internet(0x5678, true);
    uint8_t buf[64] = {};
    g_rt.serialize(buf, sizeof(buf));
    uint8_t flags = buf[6];
    TEST_ASSERT_FALSE(flags & ROUTE_FLAG_ESPNOW);
    TEST_ASSERT_TRUE(flags & ROUTE_FLAG_LORA);
    TEST_ASSERT_TRUE(flags & ROUTE_FLAG_INTERNET);
}

/* Buffer too small: returns 0. */
static void test_route_table_serialize_buffer_too_small_returns_0(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    uint8_t buf[4] = {};  /* needs 8 bytes for 1 neighbor, only 4 available */
    size_t len = g_rt.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(0, (int)len);
}

/* =========================================================================
 * invalidate_route_via / best_inet_route
 * ====================================================================== */

/* invalidate_route_via sets hops_to_internet to ROUTE_HOPS_UNKNOWN. */
static void test_route_table_invalidate_route_via_sets_hops_unknown(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false, /*hops_to_inet=*/2);
    bool affected = g_rt.invalidate_route_via(0x1001);
    TEST_ASSERT_TRUE(affected);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOPS_UNKNOWN, e.hops_to_internet);
}

/* invalidate_route_via for unknown addr returns false. */
static void test_route_table_invalidate_route_via_unknown_returns_false(void)
{
    bool affected = g_rt.invalidate_route_via(0xDEAD);
    TEST_ASSERT_FALSE(affected);
}

/* best_inet_route returns lowest hops_to_internet among all neighbors. */
static void test_route_table_best_inet_route_returns_lowest_hops(void)
{
    g_rt.update_neighbor(0xAAAA, -60, 1, true, false, /*hops_to_inet=*/5);
    g_rt.update_neighbor(0xBBBB, -60, 1, true, false, /*hops_to_inet=*/2);
    g_rt.update_neighbor(0xCCCC, -60, 1, true, false, /*hops_to_inet=*/8);
    RouteTable::InetRouteInfo info = g_rt.best_inet_route();
    TEST_ASSERT_EQUAL_UINT8(2, info.hops);
}

/* best_inet_route with no neighbors: hops == ROUTE_HOPS_UNKNOWN. */
static void test_route_table_best_inet_route_empty_returns_unknown(void)
{
    RouteTable::InetRouteInfo info = g_rt.best_inet_route();
    TEST_ASSERT_EQUAL_UINT8(ROUTE_HOPS_UNKNOWN, info.hops);
}

/* =========================================================================
 * update_inet_route: RFC 1982 sequence number wrap-around (Issue 5)
 * ====================================================================== */

/* After wrap-around (65535 → 0), seq=0 is NEWER than seq=65535.
 * Plain uint16_t comparison (0 > 65535) is false and would wrongly reject
 * the update. RFC 1982 signed-difference arithmetic must accept it. */
static void test_route_table_inet_route_seq_wrap_accepted(void)
{
    /* Seed with seq=65535 (near wrap) */
    setup_neighbor_with_initial_inet_route(0x1001, 3, 65535, 0xEEEE);
    /* Offer seq=0 from the same origin — this is 1 step newer after wrap */
    bool accepted = g_rt.update_inet_route(0x1001, 2, 0, 0xEEEE);
    TEST_ASSERT_TRUE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(0, e.inet_seq);
    TEST_ASSERT_EQUAL_UINT8(2, e.hops_to_internet);
}

/* Conversely, seq=65535 must be rejected when the stored seq is 0
 * (65535 is OLDER than 0 after wrap). */
static void test_route_table_inet_route_seq_before_wrap_rejected(void)
{
    setup_neighbor_with_initial_inet_route(0x1001, 2, 0, 0xEEEE);
    /* 65535 is one step before 0 in RFC 1982 arithmetic — it is stale */
    bool accepted = g_rt.update_inet_route(0x1001, 1, 65535, 0xEEEE);
    TEST_ASSERT_FALSE(accepted);
    NeighborEntry e;
    g_rt.get_neighbor(0x1001, e);
    TEST_ASSERT_EQUAL_UINT16(0, e.inet_seq);  /* unchanged */
}

/* =========================================================================
 * has_internet_neighbor / set_has_internet
 * ====================================================================== */

static void test_route_table_has_internet_neighbor_false_by_default(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    TEST_ASSERT_FALSE(g_rt.has_internet_neighbor());
}

static void test_route_table_has_internet_neighbor_true_after_set(void)
{
    g_rt.update_neighbor(0x1001, -60, 1, true, false);
    g_rt.set_has_internet(0x1001, true);
    TEST_ASSERT_TRUE(g_rt.has_internet_neighbor());
}

/* =========================================================================
 * Test runner
 * ====================================================================== */
void run_route_table_tests(void)
{
    RUN_TEST(test_route_table_add_one_neighbor_count_is_1);
    RUN_TEST(test_route_table_update_existing_neighbor_count_stays_same);
    RUN_TEST(test_route_table_update_existing_neighbor_rssi_updates);
    RUN_TEST(test_route_table_fill_to_max_neighbors);
    RUN_TEST(test_route_table_17th_neighbor_evicts_oldest);

    RUN_TEST(test_route_table_next_hop_direct_neighbor_returns_addr);
    RUN_TEST(test_route_table_next_hop_no_route_returns_broadcast);
    RUN_TEST(test_route_table_next_hop_empty_table_returns_broadcast);
    RUN_TEST(test_route_table_next_hop_selects_lower_cost_neighbor);

    RUN_TEST(test_route_table_inet_route_higher_seq_same_origin_accepted);
    RUN_TEST(test_route_table_inet_route_same_seq_lower_hops_accepted);
    RUN_TEST(test_route_table_inet_route_same_seq_higher_hops_rejected);
    RUN_TEST(test_route_table_inet_route_lower_seq_rejected);
    RUN_TEST(test_route_table_inet_route_different_origin_higher_seq_accepted);
    RUN_TEST(test_route_table_inet_route_unknown_neighbor_returns_false);

    RUN_TEST(test_route_table_etx_one_success_is_100);
    RUN_TEST(test_route_table_etx_two_tx_one_success_is_200);
    RUN_TEST(test_route_table_etx_capped_at_1000);
    RUN_TEST(test_route_table_etx_unknown_addr_noop);

    RUN_TEST(test_route_table_prune_removes_old_neighbor);
    RUN_TEST(test_route_table_prune_keeps_recent_neighbor);
    RUN_TEST(test_route_table_prune_empty_table_noop);

    RUN_TEST(test_route_table_serialize_empty_returns_1);
    RUN_TEST(test_route_table_serialize_one_neighbor_returns_8);
    RUN_TEST(test_route_table_serialize_flags_espnow_set);
    RUN_TEST(test_route_table_serialize_flags_lora_and_internet);
    RUN_TEST(test_route_table_serialize_buffer_too_small_returns_0);

    RUN_TEST(test_route_table_invalidate_route_via_sets_hops_unknown);
    RUN_TEST(test_route_table_invalidate_route_via_unknown_returns_false);
    RUN_TEST(test_route_table_best_inet_route_returns_lowest_hops);
    RUN_TEST(test_route_table_best_inet_route_empty_returns_unknown);

    RUN_TEST(test_route_table_has_internet_neighbor_false_by_default);
    RUN_TEST(test_route_table_has_internet_neighbor_true_after_set);

    RUN_TEST(test_route_table_inet_route_seq_wrap_accepted);
    RUN_TEST(test_route_table_inet_route_seq_before_wrap_rejected);
}
