#pragma once

// USDCC_CORE_API marks what usdcc_core (a shared library — see its
// CMakeLists.txt) exports. Header-only parts such as Signal don't need it.
#if defined(_WIN32)
#if defined(USDCC_CORE_BUILD)
#define USDCC_CORE_API __declspec(dllexport)
#else
#define USDCC_CORE_API __declspec(dllimport)
#endif
#else
#define USDCC_CORE_API __attribute__((visibility("default")))
#endif
