# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Get version: prefer FIRMWARE_VERSION env var (set by CI), then git tag, then git describe
if(DEFINED ENV{FIRMWARE_VERSION})
    set(PROJECT_VER $ENV{FIRMWARE_VERSION})
    message(STATUS "Using FIRMWARE_VERSION from environment: ${PROJECT_VER}")
else()
    # Try to get exact tag first
    execute_process(
        COMMAND git describe --tags --exact-match
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_TAG_RESULT
    )
    if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG)
        set(PROJECT_VER ${GIT_TAG})
        message(STATUS "Using git tag: ${PROJECT_VER}")
    else()
        # Fall back to git describe
        execute_process(
            COMMAND git describe --always --dirty
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_VERSION)
            set(PROJECT_VER ${GIT_VERSION})
            message(STATUS "Using git describe: ${PROJECT_VER}")
        endif()
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-wifi-bridge)
