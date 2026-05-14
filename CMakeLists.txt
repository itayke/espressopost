cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espressopost
    VERSION 0.1.0
    LANGUAGES C CXX)

idf_build_set_property(CXX_STANDARD 17)
idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
