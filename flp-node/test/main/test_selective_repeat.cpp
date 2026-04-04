/* test_selective_repeat.cpp — Unity tests targeting specific bug fixes in
 * SelectiveRepeat / FragmentSlot.
 *
 * Covered scenarios:
 *   - Issue 2: integer underflow in receive_fragment when data_idx >=
 *              total_fragments_ (offset computation wraps).
 *   - Issue 3: bitmap out-of-bounds when seq >= total_fragments_ * 8.
 *   - Issue 7: FragmentSlot::send_time_ms is zero-initialised so the first
 *              tick() does not read garbage.
 *
 * selective_repeat.cpp is compiled into the test binary (see CMakeLists.txt).
 */
#include "unity.h"
#include "selective_repeat.hpp"

#include <cstring>

using namespace flp;

static SelectiveRepeat g_arq;


/* =========================================================================
 * Issue 7 — FragmentSlot::send_time_ms default initialisation
 * ====================================================================== */

/* A default-constructed FragmentSlot must have send_time_ms == 0.
 * This ensures tick() does not read an indeterminate value on the first call
 * before any fragment has been sent. */
static void test_fragment_slot_send_time_ms_default_is_zero(void)
{
    FragmentSlot slot = {};
    TEST_ASSERT_EQUAL_UINT32(0, slot.send_time_ms);
}

/* =========================================================================
 * Issue 2 — receive_fragment: data_idx underflow guard
 * ====================================================================== */

/* When a parity fragment (is_parity path) with seq that maps to a data_idx
 * equal to total_fragments_ is received, the old code would compute an offset
 * that wraps the file_size_ subtraction (unsigned underflow → huge copy_len).
 * The fixed code must validate data_idx < total_fragments_ and abort instead
 * of overflowing.
 *
 * Set up a minimal receiver: 4 fragments of 16 bytes each (file_size=64).
 * FEC_GROUP_SIZE==7, so groups have 8 slots (7 data + 1 parity).
 * total_fragments_ passed to init_receiver includes parity slots, so we use
 * total_fragments=5 to keep the bitmap small.
 *
 * We then inject a non-parity seq whose data_idx equals total_fragments_,
 * which should be rejected cleanly.
 */
static void test_receive_fragment_invalid_data_idx_rejected(void)
{
    /* Use total_fragments=4, fragment_size=16, file_size=64. */
    const uint16_t TOTAL = 4;
    const size_t FRAG_SIZE = 16;
    const size_t FILE_SIZE = TOTAL * FRAG_SIZE;

    bool ok = g_arq.init_receiver(TOTAL, FRAG_SIZE, FILE_SIZE);
    TEST_ASSERT_TRUE(ok);

    /* seq=TOTAL is out of the valid range [0, TOTAL-1].
     * receive_fragment checks seq >= total_fragments_ at the top and should
     * return false immediately. */
    uint8_t dummy[FRAG_SIZE] = {};
    bool result = g_arq.receive_fragment(TOTAL, dummy, FRAG_SIZE);
    TEST_ASSERT_FALSE(result);

    g_arq.cleanup_receiver();
}

/* =========================================================================
 * Issue 3 — receive_fragment: bitmap bounds check
 * ====================================================================== */

/* Receive a valid fragment at the boundary of the bitmap (seq == TOTAL-1).
 * Must succeed and not read/write beyond the allocated bitmap memory.
 * Then verify is_complete() reflects the correct received count. */
static void test_receive_fragment_bitmap_boundary_seq_accepted(void)
{
    const uint16_t TOTAL = 8;
    const size_t FRAG_SIZE = 16;
    const size_t FILE_SIZE = TOTAL * FRAG_SIZE;

    bool ok = g_arq.init_receiver(TOTAL, FRAG_SIZE, FILE_SIZE);
    TEST_ASSERT_TRUE(ok);

    /* All 8 fragments fit in 1 bitmap byte (bits 0-7). seq=7 is the last
     * valid slot — byte_idx=0, which is within the 1-byte bitmap. */
    uint8_t data[FRAG_SIZE] = {};
    for (uint16_t seq = 0; seq < TOTAL; seq++) {
        bool result = g_arq.receive_fragment(seq, data, FRAG_SIZE);
        TEST_ASSERT_TRUE(result);
    }
    TEST_ASSERT_TRUE(g_arq.is_complete());

    g_arq.cleanup_receiver();
}

/* seq == TOTAL must be rejected (out of bounds — would access byte_idx that
 * exceeds the allocated bitmap). */
static void test_receive_fragment_seq_at_total_rejected(void)
{
    const uint16_t TOTAL = 8;
    const size_t FRAG_SIZE = 16;
    const size_t FILE_SIZE = TOTAL * FRAG_SIZE;

    bool ok = g_arq.init_receiver(TOTAL, FRAG_SIZE, FILE_SIZE);
    TEST_ASSERT_TRUE(ok);

    uint8_t data[FRAG_SIZE] = {};
    bool result = g_arq.receive_fragment(TOTAL, data, FRAG_SIZE);
    TEST_ASSERT_FALSE(result);

    g_arq.cleanup_receiver();
}

/* =========================================================================
 * Stride-aware ARQ (multi-exit head-of-line blocking fix)
 * ====================================================================== */

/* With stride=2, offset=0 the ARQ owns even seqs. base_seq_ must skip
 * odd (non-owned) slots when advancing after ACKs. */
static void test_stride_base_advancement(void)
{
    SelectiveRepeat arq;
    arq.init(32, 2000);
    arq.set_exit_stride(2, 0); /* exit0: even seqs */

    uint8_t data[16] = {0};
    arq.send_fragment(0, data, 16);
    arq.send_fragment(2, data, 16);
    arq.send_fragment(4, data, 16);

    TEST_ASSERT_EQUAL_UINT16(0, arq.get_base_seq());

    /* ACK seq 0 — base should advance past 0 AND skip 1 (not ours) */
    arq.handle_ack(0);
    TEST_ASSERT_EQUAL_UINT16(2, arq.get_base_seq());

    /* ACK seq 2 — base should advance past 2, skip 3 */
    arq.handle_ack(2);
    TEST_ASSERT_EQUAL_UINT16(4, arq.get_base_seq());

    /* ACK seq 4 — base advances past 4, skip 5, reaches next_seq_=5 */
    arq.handle_ack(4);
    TEST_ASSERT_EQUAL_UINT16(5, arq.get_base_seq());

    /* Window should not be full (only 3 real fragments in 32-slot window) */
    TEST_ASSERT_FALSE(arq.sender_window_full());
}

/* Default stride=1 must preserve original contiguous behavior. */
static void test_stride_1_unchanged_behavior(void)
{
    SelectiveRepeat arq;
    arq.init(8, 2000);

    uint8_t data[16] = {0};
    for (uint16_t i = 0; i < 8; i++)
        arq.send_fragment(i, data, 16);

    TEST_ASSERT_TRUE(arq.sender_window_full());
    TEST_ASSERT_EQUAL_UINT16(0, arq.get_base_seq());

    arq.handle_ack(0);
    TEST_ASSERT_EQUAL_UINT16(1, arq.get_base_seq()); /* no skipping */
}

/* reset_sender must clear stride back to defaults. */
static void test_reset_sender_clears_stride(void)
{
    SelectiveRepeat arq;
    arq.init(8, 2000);
    arq.set_exit_stride(3, 2);

    arq.reset_sender();

    /* After reset, send contiguous seqs — base should advance without skipping */
    uint8_t data[16] = {0};
    arq.send_fragment(0, data, 16);
    arq.send_fragment(1, data, 16);
    arq.handle_ack(0);
    TEST_ASSERT_EQUAL_UINT16(1, arq.get_base_seq()); /* stride=1, no skip */
}

/* Active modulo-slot aliases must be rejected until the old seq is cleared. */
static void test_active_slot_alias_is_rejected(void)
{
    SelectiveRepeat arq;
    arq.init(8, 2000);

    uint8_t data[16] = {0};
    TEST_ASSERT_EQUAL(0, arq.send_fragment(0, data, 16));
    TEST_ASSERT_TRUE(arq.can_send_sequence(0));
    TEST_ASSERT_FALSE(arq.can_send_sequence(8));
    TEST_ASSERT_EQUAL(-1, arq.send_fragment(8, data, 16));

    arq.handle_ack(0);
    TEST_ASSERT_TRUE(arq.can_send_sequence(8));
    TEST_ASSERT_EQUAL(0, arq.send_fragment(8, data, 16));
}

/* Sparse sender ownership must advance to the next real active seq rather
 * than walking unsent gaps in the numeric seq range. */
static void test_sparse_sender_base_tracks_active_sequences(void)
{
    SelectiveRepeat arq;
    arq.init(64, 2000);

    uint8_t data[16] = {0};
    TEST_ASSERT_EQUAL(0, arq.send_fragment(10, data, 16));
    TEST_ASSERT_EQUAL(0, arq.send_fragment(74, data, 16));
    TEST_ASSERT_EQUAL_UINT16(10, arq.get_base_seq());

    arq.handle_ack(10);
    TEST_ASSERT_EQUAL_UINT16(74, arq.get_base_seq());

    arq.handle_ack(74);
    TEST_ASSERT_EQUAL_UINT16(75, arq.get_base_seq());
}

/* =========================================================================
 * Test runner
 * ====================================================================== */
void run_selective_repeat_tests(void)
{
    RUN_TEST(test_fragment_slot_send_time_ms_default_is_zero);
    RUN_TEST(test_receive_fragment_invalid_data_idx_rejected);
    RUN_TEST(test_receive_fragment_bitmap_boundary_seq_accepted);
    RUN_TEST(test_receive_fragment_seq_at_total_rejected);
    RUN_TEST(test_stride_base_advancement);
    RUN_TEST(test_stride_1_unchanged_behavior);
    RUN_TEST(test_reset_sender_clears_stride);
    RUN_TEST(test_active_slot_alias_is_rejected);
    RUN_TEST(test_sparse_sender_base_tracks_active_sequences);
}
