idf_component_register(
    SRCS
        "model.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        climate
        presets
        storage
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
