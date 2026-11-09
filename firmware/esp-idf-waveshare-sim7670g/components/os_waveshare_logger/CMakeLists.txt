idf_component_register(
    SRCS "../../logger/main.cpp" "../../board_io.cpp"
    INCLUDE_DIRS "../../logger"
    REQUIRES common sensors runtime logger stac gnss gnss_reader power upload fatfs sdmmc esp_driver_sdmmc esp_driver_uart esp_driver_gpio esp_driver_i2c esp_psram spi_flash led_strip esp_timer nvs_flash espcoredump)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
