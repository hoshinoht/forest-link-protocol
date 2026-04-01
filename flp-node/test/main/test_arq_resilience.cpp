/* test_arq_resilience.cpp — Unity tests for ARQ resilience tuning changes.
 *
 * Covers:
 *   - MAX_RETRIES raised to 12 (sender survives more NACKs before failing)
 *   - RTO_MAX_MS lowered to 5000 (backoff caps earlier for faster recovery)
 *   - NACK cooldown reduced to timeout_ms_/3 (faster gap detection)
 */
#include "unity.h"
#include "selective_repeat.hpp"
#include "esp_timer.h"

#include <cstdint>
#include <vector>

using namespace flp;

/* ── Packet logger for verifying ACK/NACK/DATA sends ────────────────────── */

struct PacketRecord
{
    uint16_t seq;
    PacketType type;
};

static std::vector<PacketRecord> g_packet_log;

static int record_packet(uint16_t dst,
                         PacketType type,
                         uint16_t seq,
                         const uint8_t *data,
                         size_t len)
{
    (void)dst;
    (void)data;
    (void)len;
    g_packet_log.push_back({seq, type});
    return 0;
}

void reset_arq_resilience(void)
{
    g_mock_time_us = 0;
    g_packet_log.clear();
}

static uint32_t count_packets(PacketType type)
{
    uint32_t count = 0;
    for (const PacketRecord &pkt : g_packet_log)
    {
        if (pkt.type == type)
        {
            count++;
        }
    }
    return count;
}

static void assert_nack_sequences(const uint16_t *expected, size_t expected_count)
{
    std::vector<uint16_t> actual;
    for (const PacketRecord &pkt : g_packet_log)
    {
        if (pkt.type == PacketType::NACK)
        {
            actual.push_back(pkt.seq);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(expected_count, static_cast<uint32_t>(actual.size()));
    for (size_t i = 0; i < expected_count; i++)
    {
        TEST_ASSERT_EQUAL_UINT16(expected[i], actual[i]);
    }
}

static void init_receiver_arq(SelectiveRepeat &arq,
                              uint32_t timeout_ms,
                              uint16_t total_fragments = 8,
                              size_t fragment_size = 16,
                              size_t file_size = 7 * 16)
{
    arq.init(32, timeout_ms);
    arq.set_send_callback(record_packet);
    bool ok = arq.init_receiver(total_fragments, fragment_size, file_size);
    TEST_ASSERT_TRUE(ok);
}

/* =========================================================================
 * Group 1: MAX_RETRIES = 12
 * ========================================================================= */

/* 11 NACKs should NOT fail the sender; the 12th should. */
static void test_sender_survives_12_retries(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};

    arq.init(32, 200);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    TEST_ASSERT_EQUAL(0, arq.send_fragment(0, data, sizeof(data)));

    for (int i = 0; i < 11; i++)
    {
        arq.handle_nack(0);
        TEST_ASSERT_FALSE(arq.is_sender_failed());
    }

    /* 12th NACK triggers failure */
    arq.handle_nack(0);
    TEST_ASSERT_TRUE(arq.is_sender_failed());
}

/* Regression: old MAX_RETRIES=5 would have failed here. */
static void test_sender_not_failed_after_5_nacks(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};

    arq.init(32, 200);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    TEST_ASSERT_EQUAL(0, arq.send_fragment(0, data, sizeof(data)));

    for (int i = 0; i < 5; i++)
    {
        arq.handle_nack(0);
    }

    TEST_ASSERT_FALSE(arq.is_sender_failed());
}

/* Verify tick() drives exactly MAX_RETRIES retransmissions then stops. */
static void test_tick_retransmits_up_to_retry_limit(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};
    uint32_t elapsed_ms = 0;
    uint32_t total_retx = 0;

    arq.init(32, 200);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    TEST_ASSERT_EQUAL(0, arq.send_fragment(0, data, sizeof(data)));

    g_packet_log.clear(); /* count retransmits only */

    for (uint32_t retry = 0; retry < MAX_RETRIES; retry++)
    {
        uint32_t backoff_ms = 200U << retry;
        if (backoff_ms > 5000U)
        {
            backoff_ms = 5000U;
        }
        elapsed_ms += backoff_ms;

        /* Just before backoff expires: no retransmit */
        g_mock_time_us = static_cast<int64_t>(elapsed_ms - 1) * 1000;
        TEST_ASSERT_EQUAL_UINT8(0, arq.tick(1));

        /* At backoff expiry: retransmit fires */
        g_mock_time_us = static_cast<int64_t>(elapsed_ms) * 1000;
        TEST_ASSERT_EQUAL_UINT8(1, arq.tick(1));
        total_retx++;
    }

    /* After MAX_RETRIES: no more retransmits */
    elapsed_ms += 5000U;
    g_mock_time_us = static_cast<int64_t>(elapsed_ms) * 1000;
    TEST_ASSERT_EQUAL_UINT8(0, arq.tick(1));

    TEST_ASSERT_EQUAL_UINT32(MAX_RETRIES, total_retx);
    TEST_ASSERT_EQUAL_UINT32(MAX_RETRIES, count_packets(PacketType::DATA));
    /* tick() doesn't set sender_failed_, only handle_nack() does */
    TEST_ASSERT_FALSE(arq.is_sender_failed());
}

/* =========================================================================
 * Group 2: RTO_MAX_MS = 5000
 * ========================================================================= */

/* With a slow link (RTT=2000ms), the RTO should still cap at 5000ms. */
static void test_rto_max_caps_at_5000ms(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};

    arq.init(32, 200);
    arq.set_send_callback(record_packet);

    /* Bootstrap adaptive RTO: send at T=0, ACK at T=2000ms.
     * SRTT=2000ms, RTTVAR=1000ms, computed RTO=2000+4*1000=6000ms.
     * Must clamp to 5000ms. */
    g_mock_time_us = 0;
    TEST_ASSERT_EQUAL(0, arq.send_fragment(0, data, sizeof(data)));

    g_mock_time_us = 2000LL * 1000;
    arq.handle_ack(0);

    /* Send a new fragment after RTO has been calibrated. */
    g_mock_time_us = 2000LL * 1000;
    TEST_ASSERT_EQUAL(0, arq.send_fragment(1, data, sizeof(data)));

    g_packet_log.clear(); /* count only retransmits for seq=1 */

    /* At T=7000ms (5000ms after send): should retransmit */
    g_mock_time_us = 6999LL * 1000;
    TEST_ASSERT_EQUAL_UINT8(0, arq.tick(1));

    g_mock_time_us = 7000LL * 1000;
    TEST_ASSERT_EQUAL_UINT8(1, arq.tick(1));
    TEST_ASSERT_EQUAL_UINT32(1, count_packets(PacketType::DATA));

    /* Next retry should also fire after 5000ms (capped), not 10000ms */
    g_mock_time_us = 11999LL * 1000;
    TEST_ASSERT_EQUAL_UINT8(0, arq.tick(1));

    g_mock_time_us = 12000LL * 1000;
    TEST_ASSERT_EQUAL_UINT8(1, arq.tick(1));
    TEST_ASSERT_EQUAL_UINT32(2, count_packets(PacketType::DATA));
}

/* Verify full backoff sequence: 200, 400, 800, 1600, 3200, 5000, 5000, ... */
static void test_rto_backoff_sequence(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};
    const uint32_t expected_intervals_ms[] =
        {200, 400, 800, 1600, 3200, 5000, 5000};
    uint32_t elapsed_ms = 0;

    arq.init(32, 200);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    TEST_ASSERT_EQUAL(0, arq.send_fragment(0, data, sizeof(data)));

    g_packet_log.clear(); /* count retransmits only */

    for (size_t i = 0;
         i < sizeof(expected_intervals_ms) / sizeof(expected_intervals_ms[0]);
         i++)
    {
        elapsed_ms += expected_intervals_ms[i];

        g_mock_time_us = static_cast<int64_t>(elapsed_ms - 1) * 1000;
        TEST_ASSERT_EQUAL_UINT8(0, arq.tick(1));

        g_mock_time_us = static_cast<int64_t>(elapsed_ms) * 1000;
        TEST_ASSERT_EQUAL_UINT8(1, arq.tick(1));
    }

    TEST_ASSERT_EQUAL_UINT32(7, count_packets(PacketType::DATA));
}

/* =========================================================================
 * Group 3: NACK cooldown = timeout_ms_ / 3
 * ========================================================================= */

/* timeout=3000ms → cooldown=1000ms. Gap NACKs fire at T=1001ms, not before. */
static void test_nack_cooldown_fires_at_one_third_timeout(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};
    const uint16_t expected_nacks[] = {0, 1, 2, 3, 4};

    init_receiver_arq(arq, 3000);

    /* Receive seq=5, creating gap [0..4].  At T=0, no NACK yet. */
    g_mock_time_us = 0;
    TEST_ASSERT_TRUE(arq.receive_fragment(5, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(0, count_packets(PacketType::NACK));

    /* T=999ms: still within cooldown, no NACKs */
    g_mock_time_us = 999LL * 1000;
    TEST_ASSERT_TRUE(arq.receive_fragment(6, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(0, count_packets(PacketType::NACK));

    /* T=1001ms: past 1000ms cooldown, NACKs fire for gap [0..4] */
    g_mock_time_us = 1001LL * 1000;
    TEST_ASSERT_TRUE(arq.receive_fragment(7, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(5, count_packets(PacketType::NACK));
    assert_nack_sequences(expected_nacks, 5);

    arq.cleanup_receiver();
}

/* timeout=900ms (< 1000ms) → cooldown = timeout_ms_ (no division). */
static void test_nack_cooldown_fallback_for_small_timeout(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};
    const uint16_t expected_nacks[] = {0, 1, 2, 3, 4};

    init_receiver_arq(arq, 900);

    g_mock_time_us = 0;
    TEST_ASSERT_TRUE(arq.receive_fragment(5, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(0, count_packets(PacketType::NACK));

    /* T=899ms: still within 900ms cooldown */
    g_mock_time_us = 899LL * 1000;
    TEST_ASSERT_TRUE(arq.receive_fragment(6, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(0, count_packets(PacketType::NACK));

    /* T=901ms: past 900ms cooldown, NACKs fire */
    g_mock_time_us = 901LL * 1000;
    TEST_ASSERT_TRUE(arq.receive_fragment(7, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(5, count_packets(PacketType::NACK));
    assert_nack_sequences(expected_nacks, 5);

    arq.cleanup_receiver();
}

/* Within the cooldown period, repeated gap detection must NOT produce NACKs. */
static void test_nack_not_sent_before_cooldown(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};
    const uint16_t expected_nacks[] = {0, 1, 2, 3, 4};

    init_receiver_arq(arq, 3000);

    /* Receive seq=5 at T=0 */
    g_mock_time_us = 0;
    TEST_ASSERT_TRUE(arq.receive_fragment(5, data, sizeof(data)));

    /* T=1001ms: first batch of NACKs fires */
    g_mock_time_us = 1001LL * 1000;
    TEST_ASSERT_TRUE(arq.receive_fragment(6, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(5, count_packets(PacketType::NACK));
    assert_nack_sequences(expected_nacks, 5);

    /* T=1500ms: within next cooldown period (1001+1000=2001ms), no new NACKs */
    g_mock_time_us = 1500LL * 1000;
    TEST_ASSERT_TRUE(arq.receive_fragment(7, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT32(5, count_packets(PacketType::NACK));

    arq.cleanup_receiver();
}

/* =========================================================================
 * Group 4: HOL blocking removal (in_flight_ counter)
 * ========================================================================= */

/* With gap at seq=0, ACKing 1-7 must NOT fill the window.  Old code would
 * report window full because base_seq_ stays at 0. */
static void test_window_not_full_with_gap(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};

    arq.init(32, 2000);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    for (uint16_t i = 0; i < 8; i++)
    {
        TEST_ASSERT_EQUAL(0, arq.send_fragment(i, data, sizeof(data)));
    }

    /* ACK 1-7, skip 0 — creates a gap at base_seq_ */
    for (uint16_t i = 1; i <= 7; i++)
    {
        arq.handle_ack(i);
    }

    /* Window should NOT be full: only 1 fragment in-flight (seq=0) */
    TEST_ASSERT_FALSE(arq.sender_window_full());
    TEST_ASSERT_EQUAL_UINT16(1, arq.sender_window_used());
}

/* Verify in_flight_ accurately tracks ACKs. */
static void test_in_flight_tracks_ack(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};

    arq.init(32, 2000);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    /* Send 4 fragments */
    for (uint16_t i = 0; i < 4; i++)
    {
        TEST_ASSERT_EQUAL(0, arq.send_fragment(i, data, sizeof(data)));
    }
    TEST_ASSERT_EQUAL_UINT16(4, arq.sender_window_used());

    /* ACK 2 of them */
    arq.handle_ack(0);
    arq.handle_ack(2);
    TEST_ASSERT_EQUAL_UINT16(2, arq.sender_window_used());

    /* Duplicate ACK should NOT double-decrement */
    arq.handle_ack(0);
    TEST_ASSERT_EQUAL_UINT16(2, arq.sender_window_used());
}

/* Verify reset_sender clears in_flight_. */
static void test_in_flight_resets(void)
{
    SelectiveRepeat arq;
    uint8_t data[16] = {0};

    arq.init(32, 2000);
    arq.set_send_callback(record_packet);

    g_mock_time_us = 0;
    arq.send_fragment(0, data, sizeof(data));
    arq.send_fragment(1, data, sizeof(data));
    TEST_ASSERT_EQUAL_UINT16(2, arq.sender_window_used());

    arq.reset_sender();
    TEST_ASSERT_EQUAL_UINT16(0, arq.sender_window_used());
    TEST_ASSERT_FALSE(arq.sender_window_full());
}

/* =========================================================================
 * Test runner
 * ========================================================================= */
void run_arq_resilience_tests(void)
{
    /* Group 1: MAX_RETRIES = 12 */
    RUN_TEST(test_sender_survives_12_retries);
    RUN_TEST(test_sender_not_failed_after_5_nacks);
    RUN_TEST(test_tick_retransmits_up_to_retry_limit);

    /* Group 2: RTO_MAX_MS = 5000 */
    RUN_TEST(test_rto_max_caps_at_5000ms);
    RUN_TEST(test_rto_backoff_sequence);

    /* Group 3: NACK cooldown = timeout_ms_ / 3 */
    RUN_TEST(test_nack_cooldown_fires_at_one_third_timeout);
    RUN_TEST(test_nack_cooldown_fallback_for_small_timeout);
    RUN_TEST(test_nack_not_sent_before_cooldown);

    /* Group 4: HOL blocking removal */
    RUN_TEST(test_window_not_full_with_gap);
    RUN_TEST(test_in_flight_tracks_ack);
    RUN_TEST(test_in_flight_resets);
}
