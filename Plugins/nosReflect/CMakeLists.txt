set(MODULE_DEPENDENCIES "nos.sys.vulkan-5.8") # nos.sys.vulkan is a soft dependency.
set(dep_idx 0)
foreach(module_name_version ${MODULE_DEPENDENCIES})
    # module_name_version: <MODULE_NAME>-<MODULE_VERSION>
    string(REPLACE "-" ";" module_name_version ${module_name_version})
    list(GET module_name_version 0 module_name)
    list(GET module_name_version 1 module_version)
    nos_get_module("${module_name}" "${module_version}" DEP_${dep_idx})
    list(APPEND MODULE_DEPENDENCIES_TARGETS ${DEP_${dep_idx}})
endforeach()
list(APPEND MODULE_DEPENDENCIES_TARGETS ${NOS_PLUGIN_SDK_TARGET})

set(INCLUDE_FOLDERS "")
nos_add_plugin("nosReflect" "${MODULE_DEPENDENCIES_TARGETS}" "${INCLUDE_FOLDERS}")

# Helpers need C++20
set_target_properties("nosReflect" PROPERTIES CXX_STANDARD 20)

# # Link flatbuffers.lib
# target_link_directories("nosReflect" PRIVATE "${NOS_SDK_DIR}/lib")
# target_link_libraries("nosReflect" PRIVATE flatbuffers)