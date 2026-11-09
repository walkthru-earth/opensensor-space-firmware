# STAC File Item builder (#142). Board neutral, opt in through oslogger::Config.
idf_component_register(
    SRCS "src/os_stac_item.cpp"
    INCLUDE_DIRS "src"
    REQUIRES common location)
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
# round2() and the statistics sums must round each operation as JavaScript
# does. Xtensa has no double FMA today, but never let the compiler fuse one.
target_compile_options(${COMPONENT_LIB} PRIVATE -ffp-contract=off)
