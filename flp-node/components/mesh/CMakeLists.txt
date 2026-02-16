idf_component_register(
    SRCS "mesh_manager.cpp" "route_table.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ble lora protocol mqtt_sn
)
