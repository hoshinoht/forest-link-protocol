idf_component_register(
    SRCS
        "test_main.cpp"
        "test_packet.cpp"
        "test_fec.cpp"
        "test_route_table.cpp"
        "test_buffer_pool.cpp"
        "test_selective_repeat.cpp"
        "../mocks/mock_impl.cpp"
        "../../components/protocol/buffer_pool.cpp"
        "../../components/protocol/selective_repeat.cpp"
    INCLUDE_DIRS
        "."
        "../mocks"
        "../../components/protocol/include"
        "../../components/mesh/include"
    REQUIRES unity
)
