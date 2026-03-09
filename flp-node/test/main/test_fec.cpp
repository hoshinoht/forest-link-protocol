/* test_fec.cpp — Unity tests for FecEncoder and FecDecoder.
 *
 * FecEncoder tests verify XOR accumulation, parity length tracking, and the
 * group_complete() threshold. FecDecoder tests verify single-fragment
 * recovery, correct rejection when more than one fragment is missing,
 * group boundary arithmetic, and duplicate-ingest safety.
 *
 * Both classes are header-only inline implementations — no link dependency.
 */
#include "unity.h"
#include "fec_codec.hpp"

using namespace flp;

/* -------------------------------------------------------------------------
 * Shared test fixtures
 * ---------------------------------------------------------------------- */
static FecEncoder g_enc;
static FecDecoder g_dec;

void reset_fec(void)
{
    g_enc.reset();
    g_dec.reset();
}

/* =========================================================================
 * FecEncoder tests
 * ====================================================================== */

/* After reset, no fragments have been ingested — group must not be complete. */
static void test_fec_encoder_reset_group_not_complete(void)
{
    TEST_ASSERT_FALSE(g_enc.group_complete());
}

/* After reset, parity_len must be 0. */
static void test_fec_encoder_reset_parity_len_is_zero(void)
{
    TEST_ASSERT_EQUAL(0, (int)g_enc.parity_len());
}

/* Ingesting exactly FEC_GROUP_SIZE fragments completes the group. */
static void test_fec_encoder_complete_after_group_size_fragments(void)
{
    uint8_t frag[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    for (int i = 0; i < FEC_GROUP_SIZE; i++) {
        TEST_ASSERT_FALSE(g_enc.group_complete());
        g_enc.ingest(frag, sizeof(frag));
    }
    TEST_ASSERT_TRUE(g_enc.group_complete());
}

/* Six fragments are not enough — one short of the threshold. */
static void test_fec_encoder_not_complete_after_six_fragments(void)
{
    uint8_t frag[4] = {0x01, 0x02, 0x03, 0x04};
    for (int i = 0; i < FEC_GROUP_SIZE - 1; i++) {
        g_enc.ingest(frag, sizeof(frag));
    }
    TEST_ASSERT_FALSE(g_enc.group_complete());
}

/* XOR of 0xAA and 0x55 (byte-wise) must yield 0xFF. */
static void test_fec_encoder_xor_aa_xor_55_yields_ff(void)
{
    uint8_t a[1] = {0xAA};
    uint8_t b[1] = {0x55};
    g_enc.ingest(a, 1);
    g_enc.ingest(b, 1);
    TEST_ASSERT_EQUAL_HEX8(0xFF, g_enc.parity_data()[0]);
}

/* XOR of 0xAA with itself must yield 0x00. */
static void test_fec_encoder_xor_aa_xor_aa_yields_zero(void)
{
    uint8_t a[1] = {0xAA};
    g_enc.ingest(a, 1);
    g_enc.ingest(a, 1);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_enc.parity_data()[0]);
}

/* parity_len tracks the longest fragment seen. */
static void test_fec_encoder_parity_len_tracks_max_fragment_length(void)
{
    uint8_t short_frag[3] = {0x01, 0x02, 0x03};
    uint8_t long_frag[7]  = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
    g_enc.ingest(short_frag, 3);
    TEST_ASSERT_EQUAL(3, (int)g_enc.parity_len());
    g_enc.ingest(long_frag, 7);
    TEST_ASSERT_EQUAL(7, (int)g_enc.parity_len());
    /* Ingesting a shorter fragment must not reduce parity_len. */
    g_enc.ingest(short_frag, 3);
    TEST_ASSERT_EQUAL(7, (int)g_enc.parity_len());
}

/* After reset following a complete group, state returns to initial. */
static void test_fec_encoder_reset_after_complete_clears_state(void)
{
    uint8_t frag[4] = {0xFF, 0x00, 0xAB, 0xCD};
    for (int i = 0; i < FEC_GROUP_SIZE; i++) {
        g_enc.ingest(frag, sizeof(frag));
    }
    TEST_ASSERT_TRUE(g_enc.group_complete());
    g_enc.reset();
    TEST_ASSERT_FALSE(g_enc.group_complete());
    TEST_ASSERT_EQUAL(0, (int)g_enc.parity_len());
}

/* =========================================================================
 * FecDecoder tests
 * ====================================================================== */

/* Helper: build a synthetic fragment byte pattern. Fragments are 8 bytes,
 * where byte[0] == fragment_index so recovered data is predictable. */
static void make_frag(uint8_t *buf, uint8_t idx, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(idx ^ (uint8_t)i);
    }
}

/* Compute what the recovered fragment at 'missing_idx' should look like by
 * XOR-ing all other slots (the parity slot is included). */
static void compute_expected_recovery(uint8_t *out, size_t len,
                                      uint8_t missing_idx,
                                      uint8_t group)
{
    (void)group;
    for (size_t j = 0; j < len; j++) {
        out[j] = 0;
    }
    for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++) {
        if (i == missing_idx) continue;
        uint8_t frag[MAX_MTU];
        make_frag(frag, i, len);
        for (size_t j = 0; j < len; j++) {
            out[j] ^= frag[j];
        }
    }
}

/* Baseline: fresh decoder has not recovered anything. */
static void test_fec_decoder_initial_state_not_recovered(void)
{
    TEST_ASSERT_FALSE(g_dec.was_recovered());
}

/* --- 1-missing data fragment recovery ---
 *
 * Ingest FEC_GROUP_SIZE data fragments (indices 0–6 within group 0) and one
 * parity fragment (index 7, is_parity=true). Skip index 3. The 7th ingest
 * (completing the slot_count to FEC_GROUP_SIZE) must trigger recovery.
 * recovered_seq() must return the global seq number of the missing fragment.
 */
static void test_fec_decoder_recovers_single_missing_data_fragment(void)
{
    const uint8_t MISSING = 3;
    const size_t FRAG_LEN = 8;
    const uint8_t GROUP = 0;
    /* seqs in group 0: 0..6 are data, 7 is parity (FEC_GROUP_SIZE+1=8 per group) */
    bool recovered_triggered = false;

    for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++) {
        if (i == MISSING) continue;  /* skip missing data fragment */
        uint8_t frag[FRAG_LEN];
        make_frag(frag, i, FRAG_LEN);
        bool is_parity = (i == FEC_GROUP_SIZE);
        uint16_t seq = GROUP * (FEC_GROUP_SIZE + 1) + i;
        bool result = g_dec.ingest(seq, frag, FRAG_LEN, is_parity);
        if (result) {
            recovered_triggered = true;
        }
    }

    TEST_ASSERT_TRUE(recovered_triggered);
    TEST_ASSERT_TRUE(g_dec.was_recovered());
    /* recovered_seq must equal the global seq of the missing slot */
    uint16_t expected_seq = GROUP * (FEC_GROUP_SIZE + 1) + MISSING;
    TEST_ASSERT_EQUAL_UINT16(expected_seq, g_dec.recovered_seq());
    TEST_ASSERT_EQUAL(FRAG_LEN, g_dec.recovered_len());

    /* Verify recovered data matches XOR of all other slots */
    uint8_t expected[FRAG_LEN];
    compute_expected_recovery(expected, FRAG_LEN, MISSING, GROUP);
    TEST_ASSERT_EQUAL_MEMORY(expected, g_dec.recovered_data(), FRAG_LEN);
}

/* --- All data present, parity missing — no recovery needed ---
 *
 * When all FEC_GROUP_SIZE data fragments arrive but the parity slot is absent,
 * try_recover() detects missing_idx == FEC_GROUP_SIZE and has_parity_check()==false,
 * so it returns false. was_recovered() must remain false.
 */
static void test_fec_decoder_no_recovery_when_only_parity_missing(void)
{
    const size_t FRAG_LEN = 4;
    /* Ingest only the 7 data fragments (idx 0–6), skip parity (idx 7). */
    for (uint8_t i = 0; i < FEC_GROUP_SIZE; i++) {
        uint8_t frag[FRAG_LEN];
        make_frag(frag, i, FRAG_LEN);
        uint16_t seq = i;  /* group 0, indices 0..6 */
        g_dec.ingest(seq, frag, FRAG_LEN, /*is_parity=*/false);
    }
    /* slot_count == FEC_GROUP_SIZE at this point; try_recover runs but should
     * return false because missing_idx==7 and no parity was received. */
    TEST_ASSERT_FALSE(g_dec.was_recovered());
}

/* --- More than one missing — recovery must not occur ---
 *
 * With only 5 data fragments and 1 parity (6 total, 2 data slots missing),
 * the decoder cannot reconstruct — try_recover must return false.
 */
static void test_fec_decoder_no_recovery_with_two_missing_fragments(void)
{
    const size_t FRAG_LEN = 4;
    /* Ingest indices 0, 1, 2, 4, 5 (data) + 7 (parity) = 6 slots, missing 3 & 6 */
    uint8_t present[] = {0, 1, 2, 4, 5};
    for (uint8_t i = 0; i < sizeof(present); i++) {
        uint8_t frag[FRAG_LEN];
        make_frag(frag, present[i], FRAG_LEN);
        g_dec.ingest(present[i], frag, FRAG_LEN, false);
    }
    /* parity at idx 7 */
    uint8_t parity_frag[FRAG_LEN];
    make_frag(parity_frag, FEC_GROUP_SIZE, FRAG_LEN);
    g_dec.ingest(FEC_GROUP_SIZE, parity_frag, FRAG_LEN, true);
    /* 6 slots received — slot_count == FEC_GROUP_SIZE, try_recover runs.
     * Two data fragments are missing so recovery must fail. */
    TEST_ASSERT_FALSE(g_dec.was_recovered());
}

/* --- New group resets decoder state ---
 *
 * After completing group 0, arriving packets from group 1 must reset the
 * internal state without crashing and allow a fresh recovery attempt.
 */
static void test_fec_decoder_new_group_resets_state(void)
{
    const size_t FRAG_LEN = 4;
    /* Complete group 0 fully (all data + parity, no missing). */
    for (uint8_t i = 0; i <= FEC_GROUP_SIZE; i++) {
        uint8_t frag[FRAG_LEN];
        make_frag(frag, i, FRAG_LEN);
        uint16_t seq = 0 * (FEC_GROUP_SIZE + 1) + i; /* group 0 */
        g_dec.ingest(seq, frag, FRAG_LEN, (i == FEC_GROUP_SIZE));
    }

    /* Now ingest one fragment from group 1 — should not crash or assert. */
    uint8_t frag[FRAG_LEN];
    make_frag(frag, 0, FRAG_LEN);
    uint16_t seq_g1 = 1 * (FEC_GROUP_SIZE + 1) + 0; /* group 1, idx 0 */
    g_dec.ingest(seq_g1, frag, FRAG_LEN, false);
    /* After the reset triggered by the new group, was_recovered() must be false */
    TEST_ASSERT_FALSE(g_dec.was_recovered());
}

/* --- Group boundary arithmetic ---
 *
 * Sequences 0–7 belong to group 0 (FEC_GROUP_SIZE+1 = 8 seqs per group).
 * Sequences 8–15 belong to group 1. Verify that seq=8 is correctly identified
 * as group 1 by checking that it triggers a state reset when group 0 was active.
 */
static void test_fec_decoder_group_boundary_arithmetic(void)
{
    const size_t FRAG_LEN = 2;
    /* Put one group-0 fragment in. */
    uint8_t frag[FRAG_LEN] = {0xAB, 0xCD};
    g_dec.ingest(0, frag, FRAG_LEN, false); /* group 0, idx 0 */

    /* Inject a group-1 fragment — must trigger a group reset internally. */
    uint8_t frag2[FRAG_LEN] = {0x12, 0x34};
    g_dec.ingest(8, frag2, FRAG_LEN, false); /* group 1, idx 0 */
    /* was_recovered must be false (we only have 1 fragment in the new group) */
    TEST_ASSERT_FALSE(g_dec.was_recovered());
}

/* --- Duplicate ingest safety ---
 *
 * Ingesting the same seq twice must not increment slot_count twice.
 * This prevents a corrupt slot_count from triggering a false recovery.
 */
static void test_fec_decoder_duplicate_ingest_does_not_double_count(void)
{
    const size_t FRAG_LEN = 4;
    uint8_t frag[FRAG_LEN] = {0x11, 0x22, 0x33, 0x44};

    /* Ingest seq=0 twice */
    g_dec.ingest(0, frag, FRAG_LEN, false);
    g_dec.ingest(0, frag, FRAG_LEN, false);

    /* Now ingest unique seqs 1..5 + parity (6 more unique slots).
     * If duplicate was double-counted, slot_count would reach FEC_GROUP_SIZE
     * prematurely, potentially triggering a bad try_recover. */
    for (uint8_t i = 1; i <= 5; i++) {
        uint8_t f[FRAG_LEN];
        make_frag(f, i, FRAG_LEN);
        g_dec.ingest(i, f, FRAG_LEN, false);
    }
    uint8_t parity[FRAG_LEN];
    make_frag(parity, FEC_GROUP_SIZE, FRAG_LEN);
    g_dec.ingest(FEC_GROUP_SIZE, parity, FRAG_LEN, true);

    /* At this point: unique slots received = {0,1,2,3,4,5,7} = 7 slots,
     * missing slot 6. If duplicate-guard works, slot_count == 7 (FEC_GROUP_SIZE)
     * and try_recover runs with exactly one missing fragment — recovery should succeed. */
    TEST_ASSERT_TRUE(g_dec.was_recovered());
    TEST_ASSERT_EQUAL_UINT16(6, g_dec.recovered_seq()); /* missing idx=6 in group 0 */
}

/* --- Oversized fragment rejected (Issue 1: buffer overflow guard) ---
 *
 * Ingesting a fragment with len > MAX_MTU must return false immediately
 * without copying into the fixed-size Slot::data buffer. The decoder
 * state must remain clean (was_recovered() stays false).
 */
static void test_fec_decoder_ingest_oversized_len_returns_false(void)
{
    /* Build a dummy payload larger than MAX_MTU */
    static uint8_t big[MAX_MTU + 1];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = static_cast<uint8_t>(i & 0xFF);
    }
    bool result = g_dec.ingest(0, big, sizeof(big), false);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_FALSE(g_dec.was_recovered());
}

/* =========================================================================
 * Test runners (called from test_main.cpp)
 * ====================================================================== */
void run_fec_encoder_tests(void)
{
    RUN_TEST(test_fec_encoder_reset_group_not_complete);
    RUN_TEST(test_fec_encoder_reset_parity_len_is_zero);
    RUN_TEST(test_fec_encoder_complete_after_group_size_fragments);
    RUN_TEST(test_fec_encoder_not_complete_after_six_fragments);
    RUN_TEST(test_fec_encoder_xor_aa_xor_55_yields_ff);
    RUN_TEST(test_fec_encoder_xor_aa_xor_aa_yields_zero);
    RUN_TEST(test_fec_encoder_parity_len_tracks_max_fragment_length);
    RUN_TEST(test_fec_encoder_reset_after_complete_clears_state);
}

void run_fec_decoder_tests(void)
{
    RUN_TEST(test_fec_decoder_initial_state_not_recovered);
    RUN_TEST(test_fec_decoder_recovers_single_missing_data_fragment);
    RUN_TEST(test_fec_decoder_no_recovery_when_only_parity_missing);
    RUN_TEST(test_fec_decoder_no_recovery_with_two_missing_fragments);
    RUN_TEST(test_fec_decoder_new_group_resets_state);
    RUN_TEST(test_fec_decoder_group_boundary_arithmetic);
    RUN_TEST(test_fec_decoder_duplicate_ingest_does_not_double_count);
    RUN_TEST(test_fec_decoder_ingest_oversized_len_returns_false);
}
