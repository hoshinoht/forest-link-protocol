/* test_mesh_cmd.cpp — Unity tests for MeshCmd protocol constants.
 *
 * These are compile-time/runtime constant checks only. GPIO side effects for
 * LED_CONTROL are not covered here because the current test harness does not
 * provide gpio driver mocks for mesh_manager_bridge.
 */
#include "unity.h"
#include "packet.hpp"

using namespace flp;

static void test_led_control_constant_is_0x04(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x04, MeshCmd::LED_CONTROL);
}

static void test_led_control_does_not_collide_with_existing_mesh_cmd_values(void)
{
    TEST_ASSERT_NOT_EQUAL(MeshCmd::REQUEST_TELEMETRY, MeshCmd::LED_CONTROL);
    TEST_ASSERT_NOT_EQUAL(MeshCmd::REBOOT, MeshCmd::LED_CONTROL);
    TEST_ASSERT_NOT_EQUAL(MeshCmd::TOPIC_MSG, MeshCmd::LED_CONTROL);
}

void run_mesh_cmd_tests(void)
{
    RUN_TEST(test_led_control_constant_is_0x04);
    RUN_TEST(test_led_control_does_not_collide_with_existing_mesh_cmd_values);
}
