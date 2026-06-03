cmake_minimum_required(VERSION 3.16)
project(espressopost_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pure-math source from the model component. Intentionally NO ESP-IDF here —
# model_math.cpp has zero IDF includes, so the system compiler builds it
# straight. If you reach for an IDF header in model_math.{hpp,cpp}, this
# build will break — that's the design.
add_executable(test_model
    test_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/model/model_math.cpp
)

target_include_directories(test_model PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/model/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(NOT MSVC)
    # Catch2 v2 trips a few harmless -Wsign-compare in its parser; the rest of
    # the warning set is worth keeping strict on our own code.
    target_compile_options(test_model PRIVATE -Wall -Wextra -Wpedantic -Wno-sign-compare)
endif()

# Pure JSON serializer from the cloud component. Same deal — cloud_json.cpp has
# zero IDF includes by design, so it builds straight here. Separate binary
# because Catch2's CATCH_CONFIG_MAIN can define main() in only one TU.
add_executable(test_cloud
    test_cloud_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/cloud/cloud_json.cpp
)

target_include_directories(test_cloud PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/cloud
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(NOT MSVC)
    target_compile_options(test_cloud PRIVATE -Wall -Wextra -Wpedantic -Wno-sign-compare)
endif()

enable_testing()
add_test(NAME model_unit COMMAND test_model)
add_test(NAME cloud_unit COMMAND test_cloud)
