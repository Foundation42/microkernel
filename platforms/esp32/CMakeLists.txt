cmake_minimum_required(VERSION 3.16)

# WAMR ESP-IDF component
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/wamr/build-scripts/esp-idf")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mk_smoke)
