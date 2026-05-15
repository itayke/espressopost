idf_component_register(
    SRCS
        "rtc.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"
    REQUIRES
        driver
        touch
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
