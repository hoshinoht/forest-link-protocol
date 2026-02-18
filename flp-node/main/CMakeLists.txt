idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_wifi
        driver
        bt
        mqtt
        mesh
        ble
        lora
        mqtt_sn
        protocol
        uart_api
)
