idf_component_register(
    SRCS
        "display.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"
    REQUIRES
        driver
        esp_lcd
        lvgl__lvgl
        espressif__esp_lcd_co5300
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
