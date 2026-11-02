#pragma once

#include <pybind11/pybind11.h>

namespace usdcc::usd::python {

// Binds usdcc::usd::StageManager into `m`. Shared by the standalone module
// (usdcc/_usd.pyd) and the app's embedded module (_usdcc_app), so
// usdcc.usd's API is the same in a plain python and inside the app
// (docs/SCRIPTING.md §4.1).
//
// No UsdStageRefPtr ever crosses into Python here: doing so through a custom
// type_caster reaching into pxr_boost::python was tried and confirmed to
// corrupt unrelated boost::python state. Every stage StageManager opens is in
// UsdUtilsStageCache::Get() instead, only its integer cache id crosses, and
// Python gets the real stage through OpenUSD's own bindings:
//   UsdUtils.StageCache.Get().Find(Usd.StageCache.Id.FromLongInt(id))
// (usdcc/usd.py does that). Prim paths cross as strings.
void bindStageManager(pybind11::module_& m);

}  // namespace usdcc::usd::python
