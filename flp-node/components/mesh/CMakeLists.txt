idf_component_register(
    SRCS "mesh_manager.cpp" "route_table.cpp" "transfer_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espnow lora protocol mqtt_client esp_timer esp_wifi diagnostics
)
