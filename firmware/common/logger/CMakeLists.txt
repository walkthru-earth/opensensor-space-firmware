idf_component_register(
    SRCS "src/os_logger.cpp" "src/os_logger_status.cpp" "src/os_logger_provision.cpp" "src/os_logger_live.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime connectivity location power gnss
    PRIV_REQUIRES stac nvs_flash esp_timer esp_hw_support heap)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
