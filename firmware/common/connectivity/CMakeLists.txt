idf_component_register(
    SRCS "src/archive_sync.cpp" "src/ble_policy.cpp" "src/ble_sync.cpp" "src/control_sync.cpp" "src/frame_ring.cpp"
         "src/sync_service.cpp" "src/wifi_link.cpp" "src/network_time.cpp"
         "src/disconnect.cpp" "src/ota_lan.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common runtime
    PRIV_REQUIRES bt esp_pm esp_timer heap esp_wifi esp_netif esp_event lwip mdns
                  app_update esp_app_format esp_partition mbedtls)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})

# Match both endpoint and request originate timestamp before accepting SNTP.
# ESP-IDF 6.1.1's lwIP default is zero; no SDK/vendor source edit is required.
idf_component_get_property(os_lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${os_lwip_lib} PRIVATE SNTP_CHECK_RESPONSE=2)
