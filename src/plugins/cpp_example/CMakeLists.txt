# An example C++ plugin (docs/PLUGIN_SDK.md). Inside the app's build, the targets
# and variables are already there. To build it on its own, against a deployed
# app's SDK, this line finds them:
#     cmake -DUsdccPlugin_DIR=<deploy>/sdk -DUSD_INSTALL=<path>
#           -DCMAKE_PREFIX_PATH=<the app's vcpkg_installed/triplet> <this folder>
cmake_minimum_required(VERSION 3.21)
if(NOT TARGET usdcc::plugin_sdk)
    project(usdcc_cpp_example LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(UsdccPlugin REQUIRED)
endif()

usdcc_add_plugin(cpp_example
    ID org.usdcc.cpp-example
    NAME "C++ Example"
    VERSION 1.0.0
    DESCRIPTION "An example C++ plugin: a Prim Count panel, a Drop Locator tool and a menu action."
    SOURCES cpp_example.cpp)
