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

// The other libraries are DLLs too (docs/PLUGINS.md §3.2), so that plugins and the
// Python modules share one copy of them. CMake exports their functions
// (WINDOWS_EXPORT_ALL_SYMBOLS), but it can't export data for use without
// dllimport, so these mark what holds some: a Q_OBJECT class's staticMetaObject.
#if defined(_WIN32)
#if defined(USDCC_UI_BUILD)
#define USDCC_UI_API __declspec(dllexport)
#else
#define USDCC_UI_API __declspec(dllimport)
#endif
#if defined(USDCC_USD_UI_BUILD)
#define USDCC_USD_UI_API __declspec(dllexport)
#else
#define USDCC_USD_UI_API __declspec(dllimport)
#endif
#else
#define USDCC_UI_API __attribute__((visibility("default")))
#define USDCC_USD_UI_API __attribute__((visibility("default")))
#endif
