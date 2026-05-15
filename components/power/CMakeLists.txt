idf_component_register(
    SRCS
        "power.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        display
        lvgl__lvgl
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
