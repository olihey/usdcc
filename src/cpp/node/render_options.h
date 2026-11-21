#pragma once

// What usdcc_node's arguments ask of a render (docs/RENDER_NODE.md §4.1), and how they reach
// the job: each overrides its attribute in the stage's session layer, never in the file, so
// the job's snapshot has it and nothing is saved. Without widgets or a window, for the tests.

#include "usdcc/usd/render/render_job.h"

#include <pxr/base/gf/vec2i.h>
#include <pxr/usd/usd/stage.h>

#include <QCommandLineParser>
#include <QString>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::node {

struct FrameRange {
    double start = 0.0;
    double end = 0.0;
    double step = 1.0;
};

struct RenderArguments {
    std::string settings;  // a RenderSettings prim's path; empty: the stage's
    std::optional<FrameRange> frames;
    std::string renderer;
    std::string camera;
    std::optional<PXR_NS::GfVec2i> resolution;
    std::string output;  // absolute
    std::string format;  // "exr" or "png"
    std::optional<PXR_NS::GfVec2i> tiles;
    std::optional<double> timeLimit;
    std::vector<std::pair<std::string, std::string>> set;  // attribute, value as typed
};

// "12", "1-24", "1-24x2", "-5--1"; nothing for anything else.
std::optional<FrameRange> parseFrames(const QString& text);
// "1920x1080", "2x2"; both at least 1. Nothing for anything else.
std::optional<PXR_NS::GfVec2i> parseSize(const QString& text);
// A --set value as what it looks like: a bool, an int, a double, else the text.
PXR_NS::VtValue parseValue(const std::string& text);

// The render options in `parser`; an error for one that can't be read. A relative --output
// is under `workingFolder`.
QString readRenderArguments(const QCommandLineParser& parser, const QString& workingFolder, RenderArguments* arguments);

// Authors `arguments` in `stage`'s session layer and makes the request for the job: its
// RenderSettings prim (one made in the session layer when the stage has none), every frame
// of its range, rendered here only. Nothing, with `error`, when the settings, the renderer or
// the camera can't be used: no camera anywhere is one, as there's no viewport to borrow from.
std::optional<usdcc::render::JobRequest> prepareRenderRequest(const PXR_NS::UsdStageRefPtr& stage,
                                                              const RenderArguments& arguments, std::string* error);

}  // namespace usdcc::node
