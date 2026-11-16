// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <stddef.h>
#if defined(_WIN32)
#  if defined(USDCC_BLENDER_BACKEND_BUILD)
#    define USDCC_BLENDER_API __declspec(dllexport)
#  else
#    define USDCC_BLENDER_API __declspec(dllimport)
#  endif
#else
#  define USDCC_BLENDER_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
// Diagnostic API: no Blender/Qt/USD types or allocation ownership cross the ABI.
// Return 0 on success. Always NUL-terminate a valid caller-owned report buffer.
USDCC_BLENDER_API const char* usdcc_blender_mesh_build(void);
USDCC_BLENDER_API int usdcc_blender_mesh_probe(unsigned iterations, char* report, size_t capacity);
#ifdef __cplusplus
}
#endif
