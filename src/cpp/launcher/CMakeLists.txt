# usdcc.exe at the top of a deploy, which starts bin\usdcc.exe (launcher.cpp,
# docs/UPDATES.md §3). Windows only, and only with Velopack: it's what the
# installer's shortcuts start and what Velopack runs for its hooks.
#
# Its own folder in the build, as it has the app's name; tools/deploy.py copies
# it to the top of the deploy. Its PDB is usdcc_launcher.pdb, so it doesn't
# take the place of the app's usdcc.pdb in the symbols.
add_executable(usdcc_launcher WIN32 launcher.cpp launcher.rc)
set_target_properties(usdcc_launcher PROPERTIES OUTPUT_NAME usdcc PDB_NAME usdcc_launcher)
# Velopack's static library, built with the static C runtime: so is this, in
# every configuration (its release one, as Velopack's is), to keep one runtime.
target_link_libraries(usdcc_launcher PRIVATE usdcc::velopack_static)
set_target_properties(usdcc_launcher PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
# The version header the app's CMakeLists writes (the icon is the app's, by path).
set_property(SOURCE launcher.rc APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/src/cpp/app/generated")
target_compile_definitions(usdcc_launcher PRIVATE UNICODE _UNICODE)
# Built with the app, so a deploy always has both.
add_dependencies(usdcc usdcc_launcher)
