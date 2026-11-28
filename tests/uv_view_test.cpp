// The UV View plugin's view (docs/PRIMVAR_VIEWER.md §18): its tab in the Primvars panel where it
// applies, the layout's summary, the checker and a UDIM texture behind it, hover and picking linked
// to the stage's primvar elements, the canvas's mouse, and what a session keeps. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "uv_view.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/primvar_views.h"
#include "usdcc/usd/primvars_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QWheelEvent>

#include <functional>
#include <memory>
#include <string>

using namespace usdcc::usd;
using usdcc::uv::UvView;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
bool waitFor(const std::function<bool()>& done, int ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > ms) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

void settle() {
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

QStringList tabNames(QTabWidget* tabs) {
    QStringList names;
    for (int i = 0; i < tabs->count(); ++i) names << tabs->tabText(i);
    return names;
}

// Two quads whose UVs are apart, in 1001 and 1002; a texture for each tile, red and blue, that
// the bound material reads with st.
const char* const kStage = R"(#usda 1.0
def Mesh "Grid" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Mat>
    int[] faceVertexCounts = [4, 4]
    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]
    texCoord2f[] primvars:st = [(0.1, 0.1), (0.9, 0.1), (0.9, 0.9), (0.1, 0.9), (1.1, 0.1), (1.9, 0.1), (1.9, 0.9), (1.1, 0.9)] (
        interpolation = "faceVarying"
    )
    float[] primvars:dirt = [0, 1, 2, 3, 4, 5] (
        interpolation = "vertex"
    )
}

def Xform "Empty"
{
}

def Material "Mat"
{
    token outputs:surface.connect = </Mat/Surface.outputs:surface>

    def Shader "Surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor.connect = </Mat/Paint.outputs:rgb>
        token outputs:surface
    }

    def Shader "Paint"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @paint.<UDIM>.png@
        float2 inputs:st.connect = </Mat/Read.outputs:result>
        float3 outputs:rgb
    }

    def Shader "Read"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname = "st"
        float2 outputs:result
    }
}
)";

struct Fixture {
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()), dir.filePath(QStringLiteral("ui.ini"))};
    StageManager stages;
    std::shared_ptr<UndoManager> undo = std::make_shared<UndoManager>();
    StageRefPtr stage;
    Fixture() {
        QStandardPaths::setTestModeEnabled(true);
        const auto png = [&](int tile, const QColor& color) {
            QImage image(16, 16, QImage::Format_RGB32);
            image.fill(color);
            return image.save(dir.filePath(QStringLiteral("paint.%1.png").arg(tile)));
        };
        QFile file(dir.filePath(QStringLiteral("uv.usda")));
        if (!png(1001, Qt::red) || !png(1002, Qt::blue) || !file.open(QIODevice::WriteOnly)) return;
        file.write(kStage);
        file.close();
        stage = stages.openStage(file.fileName().toStdString());
        // The view as the plugin adds it.
        PrimvarViews::instance().add({QString::fromLatin1(usdcc::uv::kUvViewId), QStringLiteral("UV"),
                                      [](QWidget* parent) -> PrimvarView* { return new UvView(parent); }, &usdcc::uv::uvViewApplies});
    }
    ~Fixture() {
        PrimvarViews::instance().remove(QString::fromLatin1(usdcc::uv::kUvViewId));
        if (stage) stages.closeStage(stage);
    }
    void select(std::vector<SdfPath> paths) {
        stages.setSelectedPaths(stage, std::move(paths));
        settle();
    }
};

UvView* uvViewOf(PrimvarsViewPanel& panel) {
    auto* view = qobject_cast<UvView*>(panel.view(QString::fromLatin1(usdcc::uv::kUvViewId)));
    if (view) waitFor([view]() { return !view->isComputing() && view->layout(); });
    return view;
}
}  // namespace

USDCC_TEST(uv_view_tab) {
    Fixture fixture;
    CHECK(fixture.stage, "the stage opens");
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/Grid")});
    fixture.stage->setPrimvarFocus("st");
    settle();
    CHECK(tabNames(panel.tabs()).contains("UV"), "a UV tab for st: %s", qPrintable(tabNames(panel.tabs()).join(", ")));
    fixture.stage->setPrimvarFocus("dirt");
    settle();
    CHECK(!tabNames(panel.tabs()).contains("UV"), "none for a float");
    fixture.select({SdfPath("/Empty")});
    fixture.stage->setPrimvarFocus("st");
    settle();
    CHECK(!tabNames(panel.tabs()).contains("UV"), "nor without a mesh");
    fixture.select({SdfPath("/Empty"), SdfPath("/Grid")});
    CHECK(tabNames(panel.tabs()).contains("UV"), "a mesh anywhere in the selection");
    PrimvarViews::instance().remove(QString::fromLatin1(usdcc::uv::kUvViewId));
    settle();
    CHECK(!tabNames(panel.tabs()).contains("UV") && !panel.view(QString::fromLatin1(usdcc::uv::kUvViewId)), "and it goes with the plugin");
}

USDCC_TEST(uv_view_layout_and_background) {
    Fixture fixture;
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/Grid")});
    fixture.stage->setPrimvarFocus("st");
    settle();
    UvView* view = uvViewOf(panel);
    CHECK(view && view->layout() && view->layout()->faces() == 2, "laid out");
    if (!view || !view->layout()) return;
    const QString summary = view->summary();
    CHECK(summary.contains("Grid · st, faceVarying") && summary.contains("2 faces") && summary.contains("UDIM tiles 1001, 1002") &&
              summary.contains("1 seam edge"),
          "the summary: %s", qPrintable(summary));
    CHECK(view->edgeLines().size() == 8 && view->seamLines().size() == 2, "eight edges, the seam's two sides");

    CHECK(view->background() == UvView::Background::Checker && view->tileImage(1002) && !view->tileImage(1002)->isNull(), "the checker by default");
    CHECK(view->textures().size() == 1 && view->textures().front().file.find("paint.<UDIM>.png") != std::string::npos,
          "the material's texture: %s", view->textures().empty() ? "" : view->textures().front().file.c_str());
    view->setBackground(UvView::Background::Texture, 0);
    CHECK(waitFor([view]() { return !view->isLoadingTexture(); }), "the tiles are read");
    const QImage* red = view->tileImage(1001);
    const QImage* blue = view->tileImage(1002);
    CHECK(red && blue && QColor(red->pixel(8, 8)) == QColor(Qt::red) && QColor(blue->pixel(8, 8)) == QColor(Qt::blue), "each tile its own");
    CHECK(!view->tileImage(1011), "none for a tile there isn't");
    view->setBackground(UvView::Background::None);
    CHECK(!view->tileImage(1001), "nothing behind");

    // A session keeps what's behind and what's shown.
    view->setBackground(UvView::Background::Texture, 0);
    view->setShowsSeams(false);
    const QJsonObject state = view->saveState();
    UvView other;
    other.restoreState(state);
    CHECK(other.background() == UvView::Background::Texture && !other.showsSeams() && other.showsProblems(), "restored");

    // Edits are followed.
    const UsdAttribute st = fixture.stage->usdStage()->GetPrimAtPath(SdfPath("/Grid")).GetAttribute(TfToken("primvars:st"));
    VtVec2fArray values;
    st.Get(&values);
    std::swap(values[1], values[3]);  // the first face turned over
    st.Set(values);
    CHECK(waitFor([view]() { return view->layout() && view->layout()->flipped == 1; }), "an edit is laid out again");
    CHECK(view->summary().contains("1 flipped"), "and said: %s", qPrintable(view->summary()));
}

USDCC_TEST(uv_view_hover_and_pick) {
    Fixture fixture;
    if (!fixture.stage) return;
    PrimvarsViewPanel panel(&fixture.stages, fixture.stage, fixture.undo);
    fixture.select({SdfPath("/Grid")});
    fixture.stage->setPrimvarFocus("st");
    settle();
    UvView* view = uvViewOf(panel);
    if (!view || !view->layout()) return;
    const SdfPath grid("/Grid");

    view->hoverAt(GfVec2f(1.5f, 0.5f));
    CHECK((fixture.stage->primvarElementHover() == PrimvarElements{grid, "faces", {1}}) && view->hoveredFace() == std::optional<std::size_t>(1),
          "the face under the cursor is the stage's hover");
    view->hoverAt(GfVec2f(1.0f, 0.5f));
    CHECK(fixture.stage->primvarElementHover().empty(), "between faces, none");

    view->pickAt(GfVec2f(0.5f, 0.5f), false, false);
    CHECK((fixture.stage->primvarElements() == PrimvarElements{grid, "faces", {0}}), "a click picks a face");
    view->pickAt(GfVec2f(1.5f, 0.5f), true, false);
    CHECK((fixture.stage->primvarElements().indices == std::vector<std::size_t>{0, 1}), "Shift adds");
    CHECK(view->pickedFaces().size() == 2 && view->pickedFaces()[0] && view->pickedFaces()[1], "and they show here");
    view->pickAt(GfVec2f(0.5f, 0.5f), false, true);
    CHECK((fixture.stage->primvarElements().indices == std::vector<std::size_t>{1}), "Ctrl takes one away");
    view->pickAt(GfVec2f(5, 5), false, false);
    CHECK(fixture.stage->primvarElements().empty(), "a click off the faces picks nothing");

    // The stage's elements from elsewhere: points show as their corners.
    fixture.stage->setPrimvarElements({grid, "points", {1}});
    CHECK(view->pickedCorners().size() == 2, "point 1 is two corners here (%zu)", view->pickedCorners().size());
    fixture.stage->setPrimvarElements({SdfPath("/Empty"), "points", {1}});
    CHECK(view->pickedCorners().empty(), "another prim's, none");

    // The canvas's own mouse.
    view->resize(400, 400);
    view->show();
    settle();
    view->canvas()->fit();
    const QPointF at = view->canvas()->toScreen(GfVec2f(0.5f, 0.5f));
    QMouseEvent press(QEvent::MouseButtonPress, at, view->canvas()->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view->canvas()->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->canvas(), &press);
    QApplication::sendEvent(view->canvas(), &release);
    CHECK((fixture.stage->primvarElements() == PrimvarElements{grid, "faces", {0}}), "a click on the canvas");
    const QPointF moved = view->canvas()->toScreen(GfVec2f(1.5f, 0.5f));
    QMouseEvent move(QEvent::MouseMove, moved, view->canvas()->mapToGlobal(moved), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->canvas(), &move);
    CHECK(view->hoveredFace() == std::optional<std::size_t>(1), "and a move");
    const QImage drawn = view->canvas()->grab().toImage();
    CHECK(!drawn.isNull(), "it draws");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(view->canvas(), &leave);
    CHECK(fixture.stage->primvarElementHover().empty(), "leaving drops the hover");

    // Moving the view (docs/NAVIGATION.md): the scheme's drags, the wheel, and a left drag.
    usdcc::uv::UvCanvas* canvas = view->canvas();
    canvas->fit();
    const auto send = [canvas](QEvent::Type type, const QPointF& p, Qt::MouseButton button, Qt::MouseButtons buttons,
                               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, p, canvas->mapToGlobal(p), button, buttons, modifiers);
        QApplication::sendEvent(canvas, &event);
    };
    const auto isAt = [](const QPointF& a, const QPointF& b) { return (a - b).manhattanLength() < 1e-3; };
    const GfVec2f corner(0.25f, 0.25f);
    QPointF was = canvas->toScreen(corner);
    send(QEvent::MouseButtonPress, {200, 200}, Qt::MiddleButton, Qt::MiddleButton);
    send(QEvent::MouseMove, {230, 180}, Qt::NoButton, Qt::MiddleButton);
    send(QEvent::MouseButtonRelease, {230, 180}, Qt::MiddleButton, Qt::NoButton);
    CHECK(isAt(canvas->toScreen(corner), was + QPointF(30, -20)), "a middle drag pans");

    const double scale = canvas->scale();
    const QPointF cursor(120, 90);
    const GfVec2f under = canvas->toUv(cursor);
    QWheelEvent notch(cursor, canvas->mapToGlobal(cursor), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &notch);
    CHECK(std::abs(canvas->scale() / scale - 1.0 / 0.9) < 1e-6 && isAt(canvas->toScreen(under), cursor),
          "a notch of the wheel is a ninth bigger, about the cursor: %g", canvas->scale() / scale);

    const double zoomed = canvas->scale();
    send(QEvent::MouseButtonPress, cursor, Qt::RightButton, Qt::RightButton, Qt::AltModifier);
    send(QEvent::MouseMove, cursor + QPointF(-24, 0), Qt::NoButton, Qt::RightButton, Qt::AltModifier);
    send(QEvent::MouseButtonRelease, cursor + QPointF(-24, 0), Qt::RightButton, Qt::NoButton, Qt::AltModifier);
    CHECK(std::abs(canvas->scale() / zoomed - 0.9) < 1e-6 && isAt(canvas->toScreen(under), cursor),
          "an Alt+Right drag to the left zooms out, about where it began: %g", canvas->scale() / zoomed);

    const PrimvarElements picked = fixture.stage->primvarElements();
    was = canvas->toScreen(corner);
    const QPointF onFace = canvas->toScreen(GfVec2f(1.5f, 0.5f));
    send(QEvent::MouseButtonPress, onFace, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, onFace + QPointF(20, 0), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, onFace + QPointF(20, 0), Qt::LeftButton, Qt::NoButton);
    CHECK(isAt(canvas->toScreen(corner), was + QPointF(20, 0)) && fixture.stage->primvarElements() == picked,
          "a left drag pans too, and picks nothing");
}
