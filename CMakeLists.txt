cmake_minimum_required(VERSION 3.16)
set(MOSAICO_UTILS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-utils")
set(MOSAICO_BSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-bsp")
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.application.defaults")
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_compiler.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_app_recovery"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_bundle"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_iris")
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/mosaico_idf_project.cmake")
project(servocraneo_gemini_direct VERSION 1.0.0)
# Normal applications update only through retained Vibe Mode and never install
# their own bootloader. Mirror mosaico-tools' game wrapper: keep the immutable
# base bootloader out of the default build; explicit bootloader/flash targets
# retain IDF's normal dependencies and size checks. Remove to restore IDF default.
if(TARGET bootloader)
    set_property(TARGET bootloader PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")
