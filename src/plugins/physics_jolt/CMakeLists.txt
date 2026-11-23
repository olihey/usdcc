# Jolt Physics, the first physics backend (docs/PHYSICS.md §4.1): a bundled plugin, built
# and enabled in every build, as the mesh tools are. Jolt is MIT-licensed, so unlike those
# it ships in the standard artifact.
#
#   usdcc_physics_jolt_backend  the backend, a static library the plugin and the tests share
#   physics_jolt                the plugin: registers the backend
add_library(usdcc_physics_jolt_backend STATIC jolt_backend.cpp jolt_backend.h)
target_include_directories(usdcc_physics_jolt_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(usdcc_physics_jolt_backend PUBLIC usdcc_usd PRIVATE Jolt::Jolt)
set_target_properties(usdcc_physics_jolt_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

usdcc_add_plugin(physics_jolt
    ID org.usdcc.physics-jolt
    NAME "Jolt Physics"
    VERSION 0.1.0
    DESCRIPTION "Rigid-body simulation of UsdPhysics content with Jolt Physics."
    SOURCES plugin.cpp)
target_link_libraries(physics_jolt PRIVATE usdcc_physics_jolt_backend)

# Jolt's DLL beside the plugin's (a plugin's own libraries are found next to it), and its
# licence in the plugin's folder.
set(_folder "${CMAKE_BINARY_DIR}/plugins/physics_jolt")
add_custom_command(TARGET physics_jolt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Jolt::Jolt>" "${_folder}/bin/${USDCC_BUILT_FOR_PLATFORM}"
    VERBATIM)
if(EXISTS "${Jolt_DIR}/../joltphysics/copyright")
    file(COPY "${Jolt_DIR}/../joltphysics/copyright" DESTINATION "${_folder}/licenses/joltphysics")
endif()
if(TARGET usdcc)
    # Where PluginManager::shippedFolder() looks, so a build loads it as a deployed app does.
    add_custom_target(physics_jolt_app_plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_folder}" "${USDCC_APP_INSTALL_ROOT}/plugins/physics_jolt"
        DEPENDS physics_jolt VERBATIM)
    add_dependencies(usdcc physics_jolt_app_plugin)
endif()
