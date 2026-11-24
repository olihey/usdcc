# vcpkg's own community triplet arm64-osx-dynamic, with the oldest macOS usdcc runs on
# (docs/MACOS.md): without it every package targets the SDK's version, the newest
# there is. The same as CMAKE_OSX_DEPLOYMENT_TARGET in the top-level CMakeLists.txt and
# MACOS_DEPLOYMENT_TARGET in tools/_common.py.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
