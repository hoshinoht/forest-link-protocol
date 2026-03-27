idf_component_register(
    SRCS "oled_display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_driver_i2c esp_driver_gpio esp_timer main lvgl
)

# LVGL monochrome (1-bit) config
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LV_CONF_SKIP=1
    LV_COLOR_DEPTH=1
)
