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
add_subdirectory(nosTest)
add_subdirectory(nosReflect)
add_subdirectory(nosNoise)

nos_get_targets(PLUGINS_COMMON_EXTERNAL_TARGETS "./External")
nos_group_targets("${PLUGINS_COMMON_EXTERNAL_TARGETS}" "External")
