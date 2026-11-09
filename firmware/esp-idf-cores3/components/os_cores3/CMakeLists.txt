idf_component_register(
    SRCS "../../bringup/main.cpp" "../../bringup/telemetry_logger.cpp"
    INCLUDE_DIRS "../../bringup"
    REQUIRES common sensors runtime logger stac upload m5unified fatfs sdmmc esp_driver_sdspi esp_driver_uart esp_psram spi_flash nvs_flash esp_timer espcoredump)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
