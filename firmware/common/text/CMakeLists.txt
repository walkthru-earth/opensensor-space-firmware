# Each Arabic size class is its own source, and --gc-sections keeps only the
# sizes a board's font set names.
idf_component_register(
    SRCS "src/os_text.cpp"
         "src/os_text_font.cpp"
         "src/os_arabic_fonts_s_gen.cpp"
         "src/os_arabic_fonts_l_gen.cpp"
    INCLUDE_DIRS "src")
os_pin_cxx_standard(${COMPONENT_LIB})
os_map_source_paths(${COMPONENT_LIB})
