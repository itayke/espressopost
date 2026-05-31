idf_component_register(
    SRCS
        "ui_report.cpp"
        "ui_bar.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl__lvgl
        display
        climate
        model
        presets
        rtc
        storage
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
