# Shared C++ dialect for every repo-owned ESP-IDF component.
#
# ESP-IDF 6.1 defaults chip targets to the C++26 GNU dialect, and a later SDK
# may move it again. Pin the dialect here so an SDK upgrade never changes it
# silently. Every board and fixture project includes this file before
# project.cmake, and every component we own calls os_pin_cxx_standard() right
# after idf_component_register(). Third-party components keep the SDK default.
#
# The generator expression limits the flag to C++ sources, so components that
# also compile C keep the C dialect.
#
# The dialect is also named in tools/host_cxx.py, the pms-frame-test task in
# pixi.toml and tools/lint.sh. tools/check_idf_target.py fails when they
# disagree, and it fails a board build when any repo C++ file misses this flag.
include_guard(GLOBAL)

function(os_pin_cxx_standard target)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>")
endfunction()
