idf_component_register(
    SRCS
        "ui_report.cpp"
        "ui_bar.cpp"
        "ui_connections.cpp"
        "ui_menu.cpp"
        "ui_preset_readout.cpp"
        "ui_presets.cpp"
        "ui_preset_edit.cpp"
        "ui_stepper.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl__lvgl
        display
        climate
        cloud
        model
        presets
        rtc
        storage
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
