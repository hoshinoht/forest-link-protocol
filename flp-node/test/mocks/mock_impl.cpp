/* Defines the mock time variable used by esp_timer.h mock.
 * Must be compiled into the test binary exactly once. */
#include <stdint.h>

int64_t g_mock_time_us = 0;
