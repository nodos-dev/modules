# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

cmake_minimum_required(VERSION 3.24.2)
project("NOSModules")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

if (NOT WITH_NODOS_WORKSPACE)
    message(FATAL_ERROR "This repo currently does not support builds without Nodos workspace. "
    "Place this repo under nodos-workspace/Module folder and run cmake -S ./Toolchain/CMake -B Build from workspace root.")
endif()

nos_find_sdk("1.2.0" NOS_PLUGIN_SDK_TARGET NOS_SUBSYSTEM_SDK_TARGET NOS_SDK_DIR)
nos_get_module("nos.sys.vulkan" "5.8" NOS_SYS_VULKAN_TARGET_5_8)
cmake_path(SET FLATC_EXECUTABLE "${NOS_SDK_DIR}/bin/flatc")

set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Subsystems/nosShaderCompiler/Binaries/tools/${CMAKE_SYSTEM_NAME})

set(SUBSYSTEMS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Subsystems)
add_subdirectory("Subsystems" "${CMAKE_CURRENT_BINARY_DIR}/Subsystems")
nos_get_targets(SUBSYSTEM_TARGETS "${CMAKE_CURRENT_BINARY_DIR}/Subsystems")
nos_group_targets("${SUBSYSTEM_TARGETS}" "NOS Subsystems")

add_subdirectory("Plugins" "${CMAKE_CURRENT_BINARY_DIR}/Plugins")
nos_get_targets(PLUGIN_TARGETS "${CMAKE_CURRENT_BINARY_DIR}/Plugins")
nos_group_targets("${PLUGIN_TARGETS}" "NOS Plugins")