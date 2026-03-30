idf_component_register(
    SRCS "mesh_manager.cpp"
         "mesh_manager_packets.cpp"
         "mesh_manager_bridge.cpp"
         "route_table.cpp"
         "transfer_engine.cpp"
         "transfer_engine_handlers.cpp"
         "transfer_engine_arq.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espnow lora protocol mqtt_client esp_timer esp_wifi diagnostics
)
