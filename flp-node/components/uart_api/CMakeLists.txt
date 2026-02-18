idf_component_register(
    SRCS "uart_ingest.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer mesh
)
