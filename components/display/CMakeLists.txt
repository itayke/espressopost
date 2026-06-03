idf_component_register(
    SRCS
        "display.cpp"
        "lv_port_mem.c"
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
    # lv_port_mem.c provides LVGL's lv_*_core allocator overrides
    # (LV_USE_CUSTOM_MALLOC). LVGL doesn't depend on this component, so without
    # whole-archive its object is never pulled in to satisfy those refs and the
    # link fails with undefined lv_malloc_core/etc. Force the whole archive.
    WHOLE_ARCHIVE
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
