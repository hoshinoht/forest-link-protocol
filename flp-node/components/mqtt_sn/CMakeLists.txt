idf_component_register(
    SRCS "mqtt_sn_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mqtt esp_event esp_wifi
    PRIV_REQUIRES protocol
)
