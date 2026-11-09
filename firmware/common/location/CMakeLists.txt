# Unmodified official Uber H3 v4.5.0 core. Verify every vendored byte before
# configuring the upstream public header; no host tools or generators needed.
set(h3_root "${CMAKE_CURRENT_LIST_DIR}/vendor/h3")
file(READ "${h3_root}/manifest.json" h3_manifest)
string(JSON h3_count LENGTH "${h3_manifest}" files)
math(EXPR h3_last "${h3_count} - 1")
foreach(index RANGE ${h3_last})
    string(JSON name MEMBER "${h3_manifest}" files ${index})
    string(JSON expected GET "${h3_manifest}" files "${name}")
    file(SHA256 "${h3_root}/${name}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Vendored H3 hash mismatch: ${name}")
    endif()
endforeach()
set(H3_VERSION_MAJOR 4)
set(H3_VERSION_MINOR 5)
set(H3_VERSION_PATCH 0)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${h3_root}/h3lib/include/h3api.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/include/h3api.h" @ONLY)
file(GLOB h3_sources "${h3_root}/h3lib/lib/*.c")
idf_component_register(
    SRCS "src/os_location.cpp" "src/os_h3_alloc.cpp" ${h3_sources}
    INCLUDE_DIRS "src"
    PRIV_INCLUDE_DIRS "${h3_root}/h3lib/include" "${CMAKE_CURRENT_BINARY_DIR}/include")
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
# H3_ALLOC_PREFIX sends every H3 malloc, calloc, realloc and free to
# src/os_h3_alloc.cpp, which uses PSRAM (H3 alloc.h). alloc.h joins the names
# with TJOIN, which h3api.h defines only under H3_PREFIX, so H3_PREFIX is set
# empty, as the upstream v4.5.0 CMakeLists.txt always does. Exported names stay
# unprefixed. Compile definitions only, no vendored byte changes.
target_compile_definitions(${COMPONENT_LIB} PRIVATE H3_PREFIX= H3_ALLOC_PREFIX=os_h3_)
# ESP-IDF's section GC drops unused polygon/traversal/export functions. Do not
# WHOLE_ARCHIVE this component or patch H3's indexing/hierarchy algorithm.
target_compile_options(${COMPONENT_LIB} PRIVATE -ffunction-sections -fdata-sections)
