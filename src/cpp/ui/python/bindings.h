#ifndef USDCC_UI_BINDINGS_H
#define USDCC_UI_BINDINGS_H

// Aggregates every usdcc::ui header exposed to Python via Shiboken6. Add new
// headers here and a matching <object-type> in typesystem.xml to bind a new
// class.
//
// ViewPanel is deliberately NOT here: its constructor now requires
// usdcc::usd::StageManager*/StageRefPtr (pybind11 types, see
// src/cpp/usd/python/bindings.cpp), and there's no bridge between pybind11
// and this Shiboken6 pipeline — see docs/PLAN.md §6 "ViewPanel: mandatory
// Stage association" for the full explanation and acknowledged regression.
#include "usdcc/ui/side_panel.h"

#endif
