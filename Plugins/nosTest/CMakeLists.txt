# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

nos_generate_flatbuffers("${CMAKE_CURRENT_SOURCE_DIR}/Config" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "cpp" "" nosTest_generated)

if(NOT TARGET glfw)
	set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs" FORCE)
	set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build the GLFW test programs" FORCE)
	set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation" FORCE)
	set(GLFW_INSTALL OFF CACHE BOOL "Generate installation target" FORCE)
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/External/glfw)	
	message("External: ${CMAKE_CURRENT_SOURCE_DIR}/External/")
	nos_get_targets(NOSTEST_EXTERNAL_TARGETS "${CMAKE_CURRENT_SOURCE_DIR}/External/glfw")
	nos_group_targets("${NOSTEST_EXTERNAL_TARGETS}" "External")
endif()

list(APPEND DEPENDENCIES glfw ${NOS_SYS_VULKAN_TARGET_5_8} ${NOS_PLUGIN_SDK_TARGET} nosTest_generated)
list(APPEND INCLUDE_FOLDERS ${CMAKE_CURRENT_SOURCE_DIR}/External/glfw)
nos_add_plugin("nosTest" "${DEPENDENCIES}" "${INCLUDE_FOLDERS}")
