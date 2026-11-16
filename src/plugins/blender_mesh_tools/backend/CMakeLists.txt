# Our build adapter is BSD-3-Clause. Compiled Blender files retain their upstream
# licenses; this standalone backend/harness is distributed under the applicable GPL.
cmake_minimum_required(VERSION 3.21)
project(usdcc_blender_mesh_backend VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    message(FATAL_ERROR "The Blender mesh backend does not support macOS yet")
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
    add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0 NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    add_compile_options(/bigobj /Gy /Gw /MP /Zc:inline /J)
    add_link_options(/OPT:REF)
else()
    add_compile_options(-ffunction-sections -fdata-sections -fvisibility=hidden -funsigned-char -fno-strict-aliasing -ffp-contract=off)
    add_link_options(-Wl,--gc-sections)
endif()
set(BLENDER_SOURCE_DIR "" CACHE PATH "Verified source extracted by tools/build_blender_mesh.py")
if(NOT EXISTS "${BLENDER_SOURCE_DIR}/source/blender/bmesh/bmesh.hh")
    message(FATAL_ERROR "Run tools/build_blender_mesh.py --fetch first")
endif()
find_package(Eigen3 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

include(sources.cmake)
list(TRANSFORM BLENDER_MESH_SOURCES PREPEND "${BLENDER_SOURCE_DIR}/")
add_library(blender_mesh_subset STATIC ${BLENDER_MESH_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/generated/customdata_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/deform_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/curveprofile_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mesh_normals_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bmesh_construct_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bmesh_operators_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bmesh_opdefines_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mesh_support_subset.cc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/attribute_types_subset.cc"
    runtime.cpp)
set_property(TARGET blender_mesh_subset PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(blender_mesh_subset PUBLIC Eigen3::Eigen fmt::fmt Threads::Threads)
target_compile_definitions(blender_mesh_subset PUBLIC EIGEN_MPL2_ONLY)
if(WIN32)
    find_package(PThreads4W REQUIRED)
    target_link_libraries(blender_mesh_subset PUBLIC PThreads4W::PThreads4W)
    target_link_libraries(blender_mesh_subset PRIVATE dbghelp)
endif()
foreach(_inc IN ITEMS source/blender/bmesh source/blender/blenlib source/blender/blenkernel
        source/blender/makesdna source/blender/makesrna source/blender/blentranslation
        source/blender/blenloader source/blender/depsgraph source/blender/profiling
        source/blender/functions intern/guardedalloc intern/atomic intern/clog intern/eigen
        intern/profile intern/utfconv extern/xxhash extern/fast_float extern/rangetree extern/wcwidth)
    target_include_directories(blender_mesh_subset PUBLIC "${BLENDER_SOURCE_DIR}/${_inc}")
endforeach()
target_include_directories(blender_mesh_subset PRIVATE "${BLENDER_SOURCE_DIR}/source/blender/bmesh/intern"
    "${BLENDER_SOURCE_DIR}/source/blender/blenkernel/intern")
add_executable(usdcc_blender_mesh_probe probe.cpp)
add_library(usdcc_blender_mesh_backend SHARED probe_api.cpp mesh_api.cpp)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../../../deps/blender.json" _source_pin)
string(JSON _blender_version GET "${_source_pin}" version)
string(JSON _blender_commit GET "${_source_pin}" commit)
target_compile_definitions(usdcc_blender_mesh_backend PRIVATE USDCC_BLENDER_BACKEND_BUILD
    USDCC_BLENDER_BUILD="Blender-${_blender_version}-${_blender_commit}")
target_link_libraries(usdcc_blender_mesh_backend PRIVATE blender_mesh_subset)
set_property(TARGET usdcc_blender_mesh_backend PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
set_target_properties(usdcc_blender_mesh_backend PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
set_target_properties(usdcc_blender_mesh_backend PROPERTIES EXPORT_NAME blender_mesh_backend)
target_include_directories(usdcc_blender_mesh_backend INTERFACE "$<INSTALL_INTERFACE:include/usdcc_blender_mesh>")
target_link_libraries(usdcc_blender_mesh_probe PRIVATE usdcc_blender_mesh_backend)
install(TARGETS usdcc_blender_mesh_backend EXPORT UsdccBlenderMeshTargets
    RUNTIME DESTINATION bin LIBRARY DESTINATION bin ARCHIVE DESTINATION lib)
install(FILES probe_api.h mesh_api.h DESTINATION include/usdcc_blender_mesh)
install(EXPORT UsdccBlenderMeshTargets FILE UsdccBlenderMeshConfig.cmake
    NAMESPACE usdcc:: DESTINATION lib/cmake/UsdccBlenderMesh)
if(WIN32)
    install(FILES "${PThreads4W_DLL_DIR}/pthreadVC3.dll" DESTINATION bin)
    install(FILES $<TARGET_RUNTIME_DLLS:usdcc_blender_mesh_backend> DESTINATION bin)
else()
    set_target_properties(usdcc_blender_mesh_backend PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(IMPORTED_RUNTIME_ARTIFACTS fmt::fmt LIBRARY DESTINATION bin)
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/backend-inputs.txt"
    CONTENT "$<JOIN:$<TARGET_PROPERTY:blender_mesh_subset,SOURCES>,\n>\n")
file(WRITE "${CMAKE_BINARY_DIR}/build-toolchain.json"
    "{\"id\":\"${CMAKE_CXX_COMPILER_ID}\",\"version\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"system\":\"${CMAKE_SYSTEM_NAME}\",\"pointer_size\":${CMAKE_SIZEOF_VOID_P}}\n")
enable_testing()
add_test(NAME blender_mesh_probe COMMAND usdcc_blender_mesh_probe)
