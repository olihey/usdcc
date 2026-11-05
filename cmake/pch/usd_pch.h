// Precompiled header: the standard library and the OpenUSD headers almost every
// file of the Qt-free libraries pulls in (usdcc_usd, usdcc_tools). Used through
// usdcc_precompile_headers() in the top-level CMakeLists.txt, and only when
// USDCC_USE_PCH is on (the default).
//
// Rules, so it stays a pure speed-up:
// - Only stable third-party headers. Never a usdcc header: changing one would
//   rebuild every file of the library, and it would hide missing includes in
//   our own code.
// - Nothing that brings in <windows.h> (Hydra, GL, glf/garch): its macros (min,
//   max, near, far, ERROR, ...) would reach every file.
// - Every file still includes what it uses. A build with USDCC_USE_PCH=OFF
//   now and then checks that.
//
// Measured on 2026-11-05 (docs/PLAN.md §6 item 42): about 90% of a typical
// file's compile time was parsing these headers.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/xformable.h>
