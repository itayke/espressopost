idf_component_register(
    SRCS
        "climate.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"
    REQUIRES
        driver
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
