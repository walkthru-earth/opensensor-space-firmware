# Firmware location policy (res 8 only) and the PSRAM allocator hooks over the
# embedded-h3-location library, which idf_component.yml pins and the
# component manager fetches. The library compiles H3 itself and reads its
# allocator prefix from CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX, which each
# board sets to os_h3_ in sdkconfig.defaults. src/os_h3_alloc.cpp defines the
# os_h3_ hooks and fails the build when the prefix differs.
#
# WHOLE_ARCHIVE because only the library archive, which links after this one,
# calls the hooks. Without it GNU ld has already passed this archive when it
# meets those references and leaves them undefined. Unused hooks still go at
# --gc-sections.
idf_component_register(
    SRCS "src/os_h3_alloc.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES heap
    WHOLE_ARCHIVE)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
