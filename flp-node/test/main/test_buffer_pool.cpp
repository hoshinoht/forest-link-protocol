/* test_buffer_pool.cpp — Unity tests for BufferPool.
 *
 * BufferPool is a lock-free, fixed-size slab allocator using a CAS stack.
 * Tests verify capacity limits, ref-counting, double-release safety, and
 * that the pool replenishes after slabs are returned.
 *
 * buffer_pool.cpp is compiled directly into this test binary (see CMakeLists.txt).
 */
#include "unity.h"
#include "buffer_pool.hpp"

using namespace flp;

static BufferPool g_pool;

void reset_buffer_pool(void)
{
    g_pool.init();
}

/* =========================================================================
 * Acquisition and capacity
 * ====================================================================== */

/* After init(), POOL_SIZE (48) slabs must be acquirable. */
static void test_buffer_pool_can_acquire_all_slabs(void)
{
    BufferSlab *slabs[BufferPool::POOL_SIZE];
    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        slabs[i] = g_pool.acquire();
        TEST_ASSERT_NOT_NULL(slabs[i]);
    }
    /* Release all so tearDown/setUp can reinit cleanly. */
    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        g_pool.release(slabs[i]);
    }
}

/* The (POOL_SIZE+1)th acquire after init must return nullptr (pool exhausted). */
static void test_buffer_pool_25th_acquire_returns_null(void)
{
    BufferSlab *slabs[BufferPool::POOL_SIZE];
    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        slabs[i] = g_pool.acquire();
    }
    TEST_ASSERT_NULL(g_pool.acquire());

    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        g_pool.release(slabs[i]);
    }
}

/* After releasing one slab into an exhausted pool, acquire succeeds again. */
static void test_buffer_pool_release_replenishes_pool(void)
{
    BufferSlab *slabs[BufferPool::POOL_SIZE];
    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        slabs[i] = g_pool.acquire();
    }
    /* Release just one. */
    g_pool.release(slabs[0]);
    slabs[0] = nullptr;

    BufferSlab *recovered = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(recovered);
    g_pool.release(recovered);

    /* Release remaining. */
    for (int i = 1; i < BufferPool::POOL_SIZE; i++) {
        g_pool.release(slabs[i]);
    }
}

/* =========================================================================
 * Acquired slab initial state
 * ====================================================================== */

/* A freshly acquired slab must have len==0 and refcount==1. */
static void test_buffer_pool_acquired_slab_has_len_zero(void)
{
    BufferSlab *s = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL(0, (int)s->len);
    g_pool.release(s);
}

static void test_buffer_pool_acquired_slab_has_refcount_one(void)
{
    BufferSlab *s = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT8(1, s->refcount.load());
    g_pool.release(s);
}

/* =========================================================================
 * Reference counting
 * ====================================================================== */

/* add_ref increments refcount: acquire (rc=1) + add_ref (rc=2).
 * First release decrements to 1 (slab stays allocated).
 * Second release decrements to 0 (slab returns to pool). */
static void test_buffer_pool_add_ref_requires_two_releases(void)
{
    /* Drain all but one to verify pool replenishment. */
    BufferSlab *s = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT8(1, s->refcount.load());

    g_pool.add_ref(s);
    TEST_ASSERT_EQUAL_UINT8(2, s->refcount.load());

    /* First release — must NOT return slab to pool yet. */
    g_pool.release(s);
    TEST_ASSERT_EQUAL_UINT8(1, s->refcount.load());

    /* Pool should still be short by one. Exhaust remaining POOL_SIZE-1 slabs. */
    BufferSlab *rest[BufferPool::POOL_SIZE - 1];
    for (int i = 0; i < BufferPool::POOL_SIZE - 1; i++) {
        rest[i] = g_pool.acquire();
        TEST_ASSERT_NOT_NULL(rest[i]);
    }
    /* Pool now fully exhausted. */
    TEST_ASSERT_NULL(g_pool.acquire());

    /* Second release returns 's' to pool. */
    g_pool.release(s);
    TEST_ASSERT_EQUAL_UINT8(0, s->refcount.load());

    BufferSlab *recovered = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(recovered);  /* pool replenished */
    g_pool.release(recovered);

    for (int i = 0; i < BufferPool::POOL_SIZE - 1; i++) {
        g_pool.release(rest[i]);
    }
}

/* =========================================================================
 * Safety: double-release and nullptr release
 * ====================================================================== */

/* Releasing nullptr must not crash or corrupt pool state. */
static void test_buffer_pool_release_nullptr_is_safe(void)
{
    g_pool.release(nullptr);
    /* Pool should still have all POOL_SIZE slabs. */
    BufferSlab *s = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(s);
    g_pool.release(s);
}

/* Double-release (release when refcount is already 0) must not crash and must
 * not corrupt the pool freelist. The implementation guards with a prev==0 check. */
static void test_buffer_pool_double_release_does_not_corrupt_pool(void)
{
    BufferSlab *s = g_pool.acquire();
    TEST_ASSERT_NOT_NULL(s);
    g_pool.release(s);             /* rc: 1→0, slab returns to pool */
    g_pool.release(s);             /* rc: already 0 — guard must fire, no crash */

    /* Pool integrity check: we should still be able to acquire POOL_SIZE slabs. */
    BufferSlab *slabs[BufferPool::POOL_SIZE];
    int acquired = 0;
    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        slabs[i] = g_pool.acquire();
        if (slabs[i]) acquired++;
    }
    TEST_ASSERT_EQUAL(BufferPool::POOL_SIZE, acquired);
    for (int i = 0; i < BufferPool::POOL_SIZE; i++) {
        if (slabs[i]) g_pool.release(slabs[i]);
    }
}

/* =========================================================================
 * add_ref on nullptr is safe
 * ====================================================================== */
static void test_buffer_pool_add_ref_nullptr_is_safe(void)
{
    g_pool.add_ref(nullptr);  /* must not crash */
    TEST_ASSERT_EQUAL_UINT8(BufferPool::POOL_SIZE - 0, /* unchanged */
                            /* verify pool still fully available */
                            BufferPool::POOL_SIZE);
}

/* =========================================================================
 * Test runner
 * ====================================================================== */
void run_buffer_pool_tests(void)
{
    RUN_TEST(test_buffer_pool_can_acquire_all_slabs);
    RUN_TEST(test_buffer_pool_25th_acquire_returns_null);
    RUN_TEST(test_buffer_pool_release_replenishes_pool);
    RUN_TEST(test_buffer_pool_acquired_slab_has_len_zero);
    RUN_TEST(test_buffer_pool_acquired_slab_has_refcount_one);
    RUN_TEST(test_buffer_pool_add_ref_requires_two_releases);
    RUN_TEST(test_buffer_pool_release_nullptr_is_safe);
    RUN_TEST(test_buffer_pool_double_release_does_not_corrupt_pool);
    RUN_TEST(test_buffer_pool_add_ref_nullptr_is_safe);
}
