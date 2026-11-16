cmake_minimum_required(VERSION 3.21)
if(NOT TARGET usdcc::plugin_sdk)
    project(usdcc_blender_mesh_tools LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(UsdccPlugin REQUIRED)
endif()

set(USDCC_BLENDER_MESH_ROOT "${CMAKE_SOURCE_DIR}/external/blender-mesh-install"
    CACHE PATH "Private backend installed by tools/build_blender_mesh.py")
find_package(UsdccBlenderMesh CONFIG REQUIRED
    PATHS "${USDCC_BLENDER_MESH_ROOT}/lib/cmake/UsdccBlenderMesh" NO_DEFAULT_PATH)
if(NOT EXISTS "${USDCC_BLENDER_MESH_ROOT}/build-info.json")
    message(FATAL_ERROR "Backend inventory missing: run tools/build_blender_mesh.py")
endif()
file(READ "${USDCC_BLENDER_MESH_ROOT}/build-info.json" _backend_info)
string(JSON _mesh_abi ERROR_VARIABLE _mesh_abi_error GET "${_backend_info}" mesh_abi)
if(_mesh_abi_error OR NOT _mesh_abi EQUAL 1)
    message(FATAL_ERROR "Backend conversion ABI missing or unsupported: rerun tools/build_blender_mesh.py")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../deps/blender.json" _source_pin)
string(JSON _backend_commit GET "${_backend_info}" blender commit)
string(JSON _expected_commit GET "${_source_pin}" commit)
if(NOT _backend_commit STREQUAL _expected_commit)
    message(FATAL_ERROR "Backend source revision differs from deps/blender.json; rebuild the backend")
endif()
string(JSON _compiler_id GET "${_backend_info}" toolchain id)
string(JSON _compiler_version GET "${_backend_info}" toolchain version)
string(JSON _system GET "${_backend_info}" toolchain system)
string(JSON _pointer_size GET "${_backend_info}" toolchain pointer_size)
if(NOT _compiler_id STREQUAL CMAKE_CXX_COMPILER_ID OR NOT _compiler_version STREQUAL CMAKE_CXX_COMPILER_VERSION
   OR NOT _system STREQUAL CMAKE_SYSTEM_NAME OR NOT _pointer_size EQUAL CMAKE_SIZEOF_VOID_P)
    message(FATAL_ERROR "Blender backend toolchain/platform differs from usdcc; rebuild it with the same toolchain")
endif()
string(JSON _backend_config GET "${_backend_info}" configuration)
string(TOUPPER "${_backend_config}" _backend_config)
foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_property(TARGET usdcc::blender_mesh_backend PROPERTY MAP_IMPORTED_CONFIG_${_config} "${_backend_config}")
endforeach()

usdcc_add_plugin(blender_mesh_tools
    ID org.usdcc.blender-mesh-tools
    NAME "Mesh Tools"
    VERSION 0.1.0
    DESCRIPTION "Native Blender BMesh polygon mesh tools for OpenUSD."
    SOURCES src/plugin.cpp)

# USD lives on the app side of the C boundary. This library contains no Blender
# headers; it is shared by the optional plugin and its conversion tests.
add_library(usdcc_blender_mesh_conversion STATIC src/conversion.cpp src/conversion.h src/jobs.cpp src/jobs.h)
target_include_directories(usdcc_blender_mesh_conversion PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(usdcc_blender_mesh_conversion PUBLIC usdcc::plugin_sdk usdcc::blender_mesh_backend)
target_link_libraries(blender_mesh_tools PRIVATE usdcc_blender_mesh_conversion)
target_link_libraries(blender_mesh_tools PRIVATE usdcc::blender_mesh_backend)
set(_folder "${CMAKE_BINARY_DIR}/plugins/blender_mesh_tools")
# Refresh the private runtime even when its import library has not changed and
# the plugin itself does not need relinking.
add_custom_target(blender_mesh_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${USDCC_BLENDER_MESH_ROOT}/bin"
        "${_folder}/bin/${USDCC_BUILT_FOR_PLATFORM}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${USDCC_BLENDER_MESH_ROOT}/licenses" "${_folder}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${USDCC_BLENDER_MESH_ROOT}/build-info.json" "${_folder}/build-info.json"
    VERBATIM)
add_dependencies(blender_mesh_tools blender_mesh_runtime)
if(TARGET usdcc)
    # Match PluginManager::shippedFolder(), so direct builds load mesh tools
    # without requiring an extra command-line plugin path.
    add_custom_target(blender_mesh_app_plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_folder}"
            "$<TARGET_FILE_DIR:usdcc>/../plugins/blender_mesh_tools"
        DEPENDS blender_mesh_tools VERBATIM)
    add_dependencies(usdcc blender_mesh_app_plugin)
endif()
