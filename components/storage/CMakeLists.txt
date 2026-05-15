idf_component_register(
    SRCS
        "storage.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        climate
        joltwallet__littlefs
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
