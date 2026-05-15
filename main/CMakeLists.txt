idf_component_register(
    SRCS
        "app_main.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        climate
        display
        power
        presets
        rtc
        storage
        touch
        ui
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
