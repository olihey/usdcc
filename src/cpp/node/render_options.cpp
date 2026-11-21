#include "render_options.h"

#include "usdcc/usd/render/final_renderer.h"
#include "usdcc/usd/render/render_spec.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdRender/settings.h>

#include <QDir>
#include <QRegularExpression>

#include <algorithm>
#include <cerrno>
#include <cstdlib>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::node {

using namespace usdcc::render;

std::optional<FrameRange> parseFrames(const QString& text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*(-?\d+(?:\.\d+)?)(?:\s*-\s*(-?\d+(?:\.\d+)?)(?:\s*x\s*(\d+(?:\.\d+)?))?)?\s*$)"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;
    FrameRange range;
    range.start = match.captured(1).toDouble();
    range.end = match.captured(2).isEmpty() ? range.start : match.captured(2).toDouble();
    range.step = match.captured(3).isEmpty() ? 1.0 : match.captured(3).toDouble();
    if (range.end < range.start || range.step <= 0.0) return std::nullopt;
    return range;
}

std::optional<GfVec2i> parseSize(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*(\d+)\s*[xX×]\s*(\d+)\s*$)"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;
    const int width = match.captured(1).toInt();
    const int height = match.captured(2).toInt();
    if (width < 1 || height < 1) return std::nullopt;
    return GfVec2i(width, height);
}

VtValue parseValue(const std::string& text) {
    if (text == "true" || text == "on") return VtValue(true);
    if (text == "false" || text == "off") return VtValue(false);
    if (!text.empty()) {
        char* end = nullptr;
        errno = 0;
        const long long integer = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end && *end == '\0' && integer >= INT32_MIN && integer <= INT32_MAX) {
            return VtValue(static_cast<int>(integer));
        }
        end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end && *end == '\0') return VtValue(number);
    }
    return VtValue(text);
}

QString readRenderArguments(const QCommandLineParser& parser, const QString& workingFolder, RenderArguments* arguments) {
    RenderArguments& a = *arguments;
    a.settings = parser.value(QStringLiteral("settings")).toStdString();
    if (parser.isSet(QStringLiteral("frames"))) {
        a.frames = parseFrames(parser.value(QStringLiteral("frames")));
        if (!a.frames) return QStringLiteral("--frames %1: give 12, 1-24 or 1-24x2.").arg(parser.value(QStringLiteral("frames")));
    }
    a.renderer = parser.value(QStringLiteral("renderer")).toStdString();
    a.camera = parser.value(QStringLiteral("camera")).toStdString();
    if (parser.isSet(QStringLiteral("resolution"))) {
        a.resolution = parseSize(parser.value(QStringLiteral("resolution")));
        if (!a.resolution) {
            return QStringLiteral("--resolution %1: give WIDTHxHEIGHT, 1920x1080.").arg(parser.value(QStringLiteral("resolution")));
        }
    }
    if (parser.isSet(QStringLiteral("output"))) {
        const QString output = parser.value(QStringLiteral("output"));
        if (output.isEmpty()) return QStringLiteral("--output: give a file pattern.");
        a.output = QDir::cleanPath(QDir(workingFolder).absoluteFilePath(output)).toStdString();
    }
    if (parser.isSet(QStringLiteral("format"))) {
        a.format = parser.value(QStringLiteral("format")).toLower().toStdString();
        if (a.format != "exr" && a.format != "png") return QStringLiteral("--format: exr or png.");
    }
    if (parser.isSet(QStringLiteral("tiles"))) {
        a.tiles = parseSize(parser.value(QStringLiteral("tiles")));
        if (!a.tiles || (*a.tiles)[0] > 64 || (*a.tiles)[1] > 64) {
            return QStringLiteral("--tiles %1: give XxY, 2x2, each 1 to 64.").arg(parser.value(QStringLiteral("tiles")));
        }
    }
    if (parser.isSet(QStringLiteral("time-limit"))) {
        bool ok = false;
        const double seconds = parser.value(QStringLiteral("time-limit")).toDouble(&ok);
        if (!ok || seconds < 0.0) return QStringLiteral("--time-limit: seconds, 0 or more.");
        a.timeLimit = seconds;
    }
    for (const QString& set : parser.values(QStringLiteral("set"))) {
        const qsizetype equals = set.indexOf(QLatin1Char('='));
        if (equals <= 0) return QStringLiteral("--set %1: give attribute=value, embree:ty:maxBounces=8.").arg(set);
        a.set.emplace_back(set.left(equals).trimmed().toStdString(), set.mid(equals + 1).toStdString());
    }
    return {};
}

std::optional<JobRequest> prepareRenderRequest(const UsdStageRefPtr& stage, const RenderArguments& arguments,
                                               std::string* error) {
    const auto fail = [error](const std::string& why) -> std::optional<JobRequest> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!stage) return fail("No stage.");

    // Everything below goes into the session layer, which the job's snapshot flattens in
    // and nothing saves.
    const UsdEditContext session(stage, stage->GetSessionLayer());

    SdfPath settingsPath;
    if (!arguments.settings.empty()) {
        if (!SdfPath::IsValidPathString(arguments.settings)) return fail("--settings " + arguments.settings + " isn't a prim path.");
        settingsPath = SdfPath(arguments.settings);
        if (!UsdRenderSettings(stage->GetPrimAtPath(settingsPath))) {
            return fail("The stage has no RenderSettings prim " + arguments.settings + ".");
        }
    } else {
        settingsPath = defaultRenderSettings(stage);
        // None: one with the defaults, aimed at the stage's first camera.
        if (settingsPath.IsEmpty()) settingsPath = createRenderSettings(stage);
        if (settingsPath.IsEmpty()) return fail("Couldn't make render settings for the stage.");
    }
    const UsdRenderSettings settings(stage->GetPrimAtPath(settingsPath));
    const UsdPrim prim = settings.GetPrim();

    // The renderer first: its schema types the --set values.
    const std::string rendererId =
        arguments.renderer.empty() ? resolvedRendererId(resolveRenderSpec(stage, settingsPath, nullptr)) : arguments.renderer;
    const std::shared_ptr<FinalRenderer> renderer = FinalRenderers::instance().find(rendererId);
    if (!renderer) {
        return fail(rendererId.empty() ? "There is no renderer." : "There is no renderer '" + rendererId +
                                                                        "' (usdcc_node renderers lists them).");
    }
    if (const std::string problem = renderer->setupProblem(); !problem.empty()) {
        return fail(renderer->name() + " can't render here: " + problem);
    }

    if (!arguments.camera.empty()) {
        if (!SdfPath::IsValidPathString(arguments.camera) || !UsdGeomCamera(stage->GetPrimAtPath(SdfPath(arguments.camera)))) {
            return fail("--camera " + arguments.camera + " isn't a camera of the stage.");
        }
        settings.GetCameraRel().SetTargets({SdfPath(arguments.camera)});
    }
    if (arguments.resolution) settings.GetResolutionAttr().Set(*arguments.resolution);
    if (arguments.frames) {
        setJobOption(prim, jobattr::frameStart, VtValue(arguments.frames->start));
        setJobOption(prim, jobattr::frameEnd, VtValue(arguments.frames->end));
        setJobOption(prim, jobattr::frameStep, VtValue(arguments.frames->step));
    }
    if (!arguments.output.empty()) setJobOption(prim, jobattr::output, VtValue(arguments.output));
    if (!arguments.format.empty()) setJobOption(prim, jobattr::format, VtValue(TfToken(arguments.format)));
    if (arguments.tiles) setJobOption(prim, jobattr::tiles, VtValue(*arguments.tiles));
    if (arguments.timeLimit) setJobOption(prim, jobattr::timeLimit, VtValue(*arguments.timeLimit));
    if (!arguments.set.empty()) {
        const std::vector<RenderSettingField> schema = renderer->settingsSchema();
        for (const auto& [attribute, text] : arguments.set) {
            const auto field = std::find_if(schema.begin(), schema.end(),
                                            [&attribute](const RenderSettingField& f) { return f.attribute == attribute; });
            if (field == schema.end()) return fail(renderer->name() + " has no setting " + attribute + ".");
            if (!setFieldValue(prim, *field, parseValue(text))) {
                return fail("--set " + attribute + "=" + text + ": not a value " + attribute + " takes.");
            }
        }
    }

    // A camera the job can use: the settings' own, else the stage's first, as there's no
    // viewport here to borrow one from.
    if (resolveRenderSpec(stage, settingsPath, renderer.get()).camera.IsEmpty()) {
        SdfPath first;
        for (const UsdPrim& candidate : stage->Traverse()) {
            if (candidate.IsA<UsdGeomCamera>()) {
                first = candidate.GetPath();
                break;
            }
        }
        if (first.IsEmpty()) return fail("No camera: the render settings name none and the stage has none.");
        settings.GetCameraRel().SetTargets({first});
    }

    JobRequest request;
    request.stage = stage;
    request.settings = settingsPath;
    request.frames = JobRequest::Frames::Range;
    request.rendererOverride = arguments.renderer;
    // Only here, whatever the profile's Render on says.
    request.dispatch = PeerDispatch{};
    return request;
}

}  // namespace usdcc::node
