cmake_minimum_required(VERSION 3.16)

# Test build for the forest-link-protocol Unity test suite.
# Compile against ESP-IDF so unity component is available.
# Mock headers in mocks/ shadow real ESP-IDF headers (esp_timer.h, esp_log.h).
# Do NOT add ../components — pulling the full component tree drags in lora,
# mqtt_client, espnow etc. which need Kconfig symbols that don't exist in this
# test build. Source files are compiled directly via SRCS in main/CMakeLists.txt.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flp_tests)
