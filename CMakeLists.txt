cmake_minimum_required(VERSION 3.16)

# Personal overrides (e.g. dev Wi-Fi) go in the gitignored sdkconfig.defaults.local.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(reflbo-nm-epd-420)

# Scrub local workspace paths from compiled binaries.
set(_reflbo_prefix_maps
    "-fmacro-prefix-map=${CMAKE_CURRENT_LIST_DIR}=/workspace"
    "-ffile-prefix-map=${CMAKE_CURRENT_LIST_DIR}=/workspace"
    "-fmacro-prefix-map=/project=/workspace"
    "-ffile-prefix-map=/project=/workspace"
)
add_compile_options(${_reflbo_prefix_maps})
