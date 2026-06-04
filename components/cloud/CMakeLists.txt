idf_component_register(
    SRCS
        "cloud.cpp"
        "cloud_json.cpp"
        "cloud_portal.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_http_server
        esp-tls
        mbedtls
        nvs_flash
        console
        storage
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
