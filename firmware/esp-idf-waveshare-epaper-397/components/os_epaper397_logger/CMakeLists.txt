idf_component_register(
    SRCS "../../logger/main.cpp" "../../board_io.cpp"
         "../../display/epd_ssd1677.cpp" "../../display/bringup_screen.cpp"
    INCLUDE_DIRS "../../logger" "../../display" "../.."
    REQUIRES common sensors runtime logger connectivity stac power upload fatfs sdmmc esp_driver_sdmmc esp_driver_spi esp_driver_gpio esp_driver_i2c esp_psram spi_flash esp_timer nvs_flash espcoredump heap)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
