# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

cmake_minimum_required(VERSION 3.24.2)
project("Plugins")

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/External)

if (NOT WITH_NODOS_WORKSPACE)
    include(${SCRIPTS_DIR}/CMake/Plugins.cmake)
endif()

# Dependencies
# ------------

# Update plugins.json if you add dependencies, so that CD can create releases.

# Common dependencies
add_subdirectory(External)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/compile_shaders.py
    RESULT_VARIABLE COMPILE_SHADERS_RESULT
)
if (NOT ${COMPILE_SHADERS_RESULT} EQUAL "0")
    message(FATAL_ERROR "Failed to compile shaders. Process returned ${COMPILE_SHADERS_RESULT}.")
endif()

add_subdirectory(nosMath)
add_subdirectory(nosFilters)
add_subdirectory(nosUtilities)
add_subdirectory(nosFilesystem)
add_subdirectory(nosTest)
if (WIN32)
    add_subdirectory(nosWebRTC)
endif ()
add_subdirectory(nosTrack)
add_subdirectory(nosNoise)
if (WIN32)
    add_subdirectory(nosPyUtils)
endif()
add_subdirectory(nosReflect)
add_subdirectory(nosStrings)
add_subdirectory(nosAnimation)
add_subdirectory(nosGraphics)
add_subdirectory(nosGeometry)

nos_get_targets(PLUGINS_COMMON_EXTERNAL_TARGETS "./External")
nos_group_targets("${PLUGINS_COMMON_EXTERNAL_TARGETS}" "External")
