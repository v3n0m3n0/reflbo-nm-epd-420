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

# Force esp_app_desc.c.obj to rebuild on incremental ESP-IDF builds so
# esp_app_get_description() (__DATE__ / __TIME__) reflects the new build time.
if(NOT BOOTLOADER_BUILD AND ESP_PLATFORM AND TARGET __idf_esp_app_format)
    set(_app_desc_stamp "${CMAKE_BINARY_DIR}/esp_app_desc.stamp")
    add_custom_target(refresh_esp_app_desc
        COMMAND ${CMAKE_COMMAND} -E touch "${_app_desc_stamp}"
        BYPRODUCTS "${_app_desc_stamp}"
        COMMENT "Refreshing esp_app_desc build timestamp"
    )
    get_target_property(_eaf_dir __idf_esp_app_format SOURCE_DIR)
    set_source_files_properties("${_eaf_dir}/esp_app_desc.c"
        TARGET_DIRECTORY __idf_esp_app_format
        PROPERTIES OBJECT_DEPENDS "${_app_desc_stamp}"
    )
    add_dependencies(__idf_esp_app_format refresh_esp_app_desc)
endif()
