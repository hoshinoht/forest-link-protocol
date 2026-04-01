/* test_main.cpp — Unity test entry point for the forest-link-protocol test suite.
 *
 * For ESP-IDF Unity builds the app_main() function is the entry point.
 * UNITY_BEGIN / UNITY_END bracket all test groups. Each group's runner
 * function is declared extern and called here so individual test files
 * stay self-contained.
 */
#include "unity.h"
#include "esp_log.h"  /* no-op mock */

/* Per-file fixture resets — called from setUp() before every RUN_TEST */
void reset_fec(void);
void reset_route_table(void);
void reset_buffer_pool(void);
void reset_mesh_improvements(void);
void reset_arq_resilience(void);

/* Single global setUp/tearDown required by Unity */
void setUp(void)
{
    reset_fec();
    reset_route_table();
    reset_buffer_pool();
    reset_mesh_improvements();
    reset_arq_resilience();
}
void tearDown(void) {}

/* Forward declarations of per-file test runners */
void run_packet_tests(void);
void run_fec_encoder_tests(void);
void run_fec_decoder_tests(void);
void run_route_table_tests(void);
void run_buffer_pool_tests(void);
void run_selective_repeat_tests(void);
void run_mesh_improvement_tests(void);
void run_arq_resilience_tests(void);
void run_mesh_cmd_tests(void);

extern "C" void app_main(void)
{
    UNITY_BEGIN();

    run_packet_tests();
    run_fec_encoder_tests();
    run_fec_decoder_tests();
    run_route_table_tests();
    run_buffer_pool_tests();
    run_selective_repeat_tests();
    run_mesh_improvement_tests();
    run_arq_resilience_tests();
    run_mesh_cmd_tests();

    UNITY_END();
}
