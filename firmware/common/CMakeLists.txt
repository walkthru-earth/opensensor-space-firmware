idf_component_register(
    SRCS "src/parquet_writer.cpp" "src/parquet_reader.cpp" "src/parquet_columns.cpp"
         "src/lz4_codec.cpp"
         "src/ota_transfer.cpp" "src/boot_self_test.cpp" "src/sha256_stream.cpp"
         "src/flash_volume.cpp" "src/flash_volume_policy.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer mbedtls fatfs wear_levelling nvs_flash)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
