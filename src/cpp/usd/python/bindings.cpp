#include <pybind11/pybind11.h>

#include "usdcc/usd/python/bind_usd.h"

// usdcc/_usd.pyd: usdcc.usd's native part outside the app — a StageManager
// of its own, for a plain python. usdcc/usd.py picks this or, inside the app,
// the embedded _usdcc_app module (docs/SCRIPTING.md §4.1).
PYBIND11_MODULE(_usd, m) {
    m.doc() = "usdcc.usd's native part for a standalone python. Use usdcc.usd, not this.";
    usdcc::usd::python::bindStageManager(m);
}
