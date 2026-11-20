idf_component_register(
    SRCS "../../logger/main.cpp" "../../logger/history_restore.cpp"
         "../../board_io.cpp"
         "../../display/epd_uc8276c.cpp"
    INCLUDE_DIRS "../../logger" "../../display"
    REQUIRES epaper_ui common sensors runtime logger connectivity stac power upload fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio esp_driver_i2c esp_psram spi_flash esp_timer nvs_flash espcoredump heap)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
