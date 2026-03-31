cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flp-node)

# Workaround: GCC 14.2.0 ICE (segfault) in esp_lcd_panel_rgb.c during the IRA
# register-allocation pass at -Og.  Dropping to -O0 for the esp_lcd component
# sidesteps the compiler bug.  FLP does not use esp_lcd — this only unblocks
# the build until Espressif ships a fixed toolchain.
idf_component_get_property(lcd_lib esp_lcd COMPONENT_LIB)
target_compile_options(${lcd_lib} PRIVATE -O0)
