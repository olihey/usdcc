# vcpkg's host triplet on Apple Silicon: the tools its packages' builds run (Qt's moc and
# rcc). Static, so they need no library beside them: vcpkg hard-links what it installs from
# a staging folder, and while that is read (packed into the binary cache) macOS takes a
# running tool to be there, where a dynamic moc finds no libpcre2 (docs/MACOS.md). Release
# only, as nothing links it. The same oldest macOS as arm64-osx-dynamic.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
