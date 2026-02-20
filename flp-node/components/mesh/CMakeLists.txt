idf_component_register(
    SRCS "mesh_manager.cpp" "route_table.cpp" "transfer_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ble lora protocol mqtt_sn esp_timer diagnostics
)
