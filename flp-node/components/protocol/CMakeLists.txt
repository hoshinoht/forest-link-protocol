idf_component_register(
    SRCS "protocol_selector.cpp" "selective_repeat.cpp" "buffer_pool.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log
)
