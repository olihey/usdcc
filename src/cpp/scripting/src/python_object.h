#pragma once

// Private to usdcc_scripting_app: a Python object held by C++ that may outlive
// a single Python call (a registered panel class, a panel's Python instance).
#include <pybind11/pybind11.h>

#include <utility>

namespace usdcc::scripting {

// Releases its object with the GIL held, whichever thread or call drops the
// last reference. If Python is already finalized (the app's panel registry
// outlives the interpreter), the reference is abandoned instead, since there
// is nothing left to release it to.
struct PythonObject {
    pybind11::object object;

    explicit PythonObject(pybind11::object o) : object(std::move(o)) {}
    ~PythonObject() {
        if (Py_IsInitialized()) {
            pybind11::gil_scoped_acquire gil;
            object = pybind11::object();
        } else {
            object.release();
        }
    }
    PythonObject(const PythonObject&) = delete;
    PythonObject& operator=(const PythonObject&) = delete;
};

}  // namespace usdcc::scripting
