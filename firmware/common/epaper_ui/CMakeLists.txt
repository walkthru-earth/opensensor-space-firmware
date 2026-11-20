# The shared e-paper UI, pure C++ with no ESP-IDF API, so the host tests and
# the preview compile the same sources. Each layout table and each font set
# is its own source, and --gc-sections keeps only the ones a board's
# DisplayProfile names.
idf_component_register(
    SRCS "src/canvas.cpp" "src/chart.cpp" "src/history.cpp" "src/pages.cpp"
         "src/settings.cpp" "src/ui_model.cpp" "src/strings.cpp"
         "src/panel.cpp" "src/profile.cpp" "src/orientation.cpp"
         "src/presenter.cpp"
         "src/layout_400x300.cpp" "src/layout_300x400.cpp"
         "src/layout_800x480.cpp" "src/layout_480x800.cpp"
         "src/fonts_compact_gen.cpp" "src/fonts_large_gen.cpp"
    INCLUDE_DIRS "include")
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
