idf_component_register(
    SRCS
        "presets.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
