cmake_minimum_required(VERSION 3.23)
project(jxr_to_avif C)

set(CMAKE_C_STANDARD 17)

add_executable(jxr_to_avif main.c)
find_library(AVIF_LIBRARY avif PATHS ${PROJECT_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)
find_library(AOM_LIBRARY aom PATHS ${PROJECT_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)
find_library(SHARPYUV_LIBRARY sharpyuv PATHS ${PROJECT_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)

target_link_libraries(jxr_to_avif ${AVIF_LIBRARY} ${AOM_LIBRARY} "$<$<CONFIG:Release>:-s -static>")
