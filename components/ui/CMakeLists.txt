idf_component_register(
    SRCS
        "ui_bringup.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lvgl__lvgl
        display
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
