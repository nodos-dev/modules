# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

nos_generate_flatbuffers("${CMAKE_CURRENT_SOURCE_DIR}/Config" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "cpp" "" nosUtilities_generated)
nos_add_plugin("nosUtilities" "stb;nosUtilities_generated;${NOS_SYS_VULKAN_TARGET_5_8};${NOS_PLUGIN_SDK_TARGET}" "")
