#pragma once

// USDCC_PEERS_API marks what usdcc_peers (a shared library, see its CMakeLists.txt)
// exports.
#if defined(_WIN32)
#if defined(USDCC_PEERS_BUILD)
#define USDCC_PEERS_API __declspec(dllexport)
#else
#define USDCC_PEERS_API __declspec(dllimport)
#endif
#else
#define USDCC_PEERS_API __attribute__((visibility("default")))
#endif
