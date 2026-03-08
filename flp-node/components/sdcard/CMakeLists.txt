idf_component_register(
    SRCS "sdcard.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fatfs vfs sdmmc driver
)
