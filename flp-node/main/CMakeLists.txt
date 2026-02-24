idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_wifi
        driver
        mqtt
        mesh
        espnow
        lora
        mqtt_client
        protocol
        uart_api
        display
)
