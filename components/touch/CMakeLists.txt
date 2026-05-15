idf_component_register(
    SRCS
        "touch.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"
    REQUIRES
        driver
        esp_lcd
        lvgl__lvgl
        waveshare__esp_lcd_touch_cst9217
        display
        power
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
