set(LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/esp-idf-ssd1306/components/ssd1306")

idf_component_register(
    SRCS "oled_display.cpp"
         "${LIB_DIR}/ssd1306.c"
         "${LIB_DIR}/ssd1306_i2c_new.c"
         "${LIB_DIR}/ssd1306_spi.c"
    INCLUDE_DIRS "include" "${LIB_DIR}"
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_driver_spi esp_timer main
)

# Provide Kconfig symbols the library expects (normally from its Kconfig.projbuild)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_OFFSETX=0
    CONFIG_I2C_PORT_0=1
    CONFIG_SPI2_HOST=1
)
