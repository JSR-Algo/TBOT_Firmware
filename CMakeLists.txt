# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

option(TBOT_NATIVE_COVERAGE "Instrument firmware sources for ESP-IDF gcov coverage" OFF)
set(TBOT_HIL_PROFILE "production" CACHE STRING "Non-secret deployed firmware profile attested to HIL inspection")
if(NOT TBOT_HIL_PROFILE MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR "TBOT_HIL_PROFILE must be a non-empty safe token")
endif()
string(LENGTH "${TBOT_HIL_PROFILE}" TBOT_HIL_PROFILE_LENGTH)
if(TBOT_HIL_PROFILE_LENGTH GREATER 64)
    message(FATAL_ERROR "TBOT_HIL_PROFILE must be at most 64 bytes")
endif()

# ESP-IDF does not auto-load sdkconfig.defaults.local. Keep the production
# LCDWiki overlay in the default/CI build path so plain idf.py builds select the
# real board and software AES unless a caller explicitly supplies a build matrix.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.local")
endif()

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(TBOT_NATIVE_COVERAGE)
    idf_build_set_property(MINIMAL_BUILD OFF)
else()
    # "Trim" the build. Include the minimal set of components, main, and anything it depends on.
    idf_build_set_property(MINIMAL_BUILD ON)
endif()

set(PROJECT_VER "2.2.85")
project(xiaozhi)

if(TBOT_NATIVE_COVERAGE)
    include($ENV{IDF_PATH}/components/app_trace/project_include.cmake)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/coverage_report" TBOT_COVERAGE_REPORT_DIR)
    idf_create_coverage_report(${TBOT_COVERAGE_REPORT_DIR})
    idf_clean_coverage_report(${TBOT_COVERAGE_REPORT_DIR})
endif()
