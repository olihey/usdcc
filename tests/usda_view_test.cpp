// The USDA view (docs/USDA_VIEW.md): what each Show mode shows, colouring
// only what's on screen, refreshing after an edit (by itself for a small
// layer, by hand for the flattened stage), keeping the scroll position, and
// Find. With USDCC_BENCH=1 it also times a generated ~100 MB layer (Â§5).
// Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usda_syntax.h"
#include "usdcc/usd/usda_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UsdaViewPanel;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void wait(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QApplication::processEvents();
}

QString shown(const UsdaViewPanel& panel) { return QString::fromStdString(panel.lineView()->text()); }

// A layer of `prims` prims, each with a few attributes and an array.
std::string bigLayer(int prims) {
    std::string text = "#usda 1.0\n(\n    defaultPrim = \"World\"\n    doc = \"\"\"A generated layer\nfor timing.\"\"\"\n)\n\n";
    text += "def Xform \"World\"\n{\n";
    for (int i = 0; i < prims; ++i) {
        const std::string n = std::to_string(i);
        text += "    def Mesh \"Mesh_" + n + "\" (\n        prepend references = @./parts/part_" + n + ".usda@</Part>\n    )\n    {\n";
        text += "        float3[] extent = [(-1, -1, -1), (1, 1, 1)]\n";
        text += "        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n";
        text += "        point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]\n";
        text += "        double3 xformOp:translate = (" + n + ", 0, 0)\n";
        text += "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n    }\n";
    }
    text += "}\n";
    return text;
}
}  // namespace

USDCC_TEST(usda_view_panel) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    const UsdStageRefPtr& s = stage->usdStage();
    const SdfLayerRefPtr part = SdfLayer::CreateAnonymous("part.usda");
    SdfCreatePrimInLayer(part, SdfPath("/Part"))->SetSpecifier(SdfSpecifierDef);
    const SdfLayerRefPtr sub = SdfLayer::CreateAnonymous("sub.usda");
    SdfCreatePrimInLayer(sub, SdfPath("/FromSub"))->SetSpecifier(SdfSpecifierDef);
    s->GetRootLayer()->GetSubLayerPaths().push_back(sub->GetIdentifier());
    s->DefinePrim(SdfPath("/World"), TfToken("Xform")).GetReferences().AddReference(part->GetIdentifier(), SdfPath("/Part"));

    UsdaViewPanel panel(&stages, stage);
    panel.resize(700, 500);
    panel.show();
    wait(50);
    CHECK(shown(panel).startsWith("#usda 1.0") && shown(panel).contains("def Xform \"World\"") &&
              !shown(panel).contains("FromSub"),
          "the root layer as authored");

    UsdaViewPanel::Source source;
    source.identifier = sub->GetIdentifier();
    panel.setSource(source);
    CHECK(shown(panel).contains("FromSub") && !shown(panel).contains("World"), "a sublayer");
    source.kind = UsdaViewPanel::Source::Kind::FlattenedLayerStack;
    panel.setSource(source);
    CHECK(shown(panel).contains("FromSub") && shown(panel).contains("World") && shown(panel).contains("references"),
          "the flattened layer stack: both layers, the reference kept");
    source.kind = UsdaViewPanel::Source::Kind::FlattenedStage;
    panel.setSource(source);
    CHECK(shown(panel).contains("FromSub") && !shown(panel).contains("references"),
          "the flattened stage: the reference composed in");

    // The flattened stage refreshes by hand.
    s->DefinePrim(SdfPath("/Late"));
    wait(700);
    CHECK(panel.isOutOfDate() && !shown(panel).contains("Late"), "flattened stage: out of date, not refreshed");
    panel.refresh();
    CHECK(!panel.isOutOfDate() && shown(panel).contains("Late"), "until Refresh");

    // A small layer refreshes itself after an edit, keeping its scroll position.
    panel.setSource(UsdaViewPanel::Source());
    for (int i = 0; i < 300; ++i) s->DefinePrim(SdfPath("/World/P" + std::to_string(i)));
    panel.refresh();
    panel.lineView()->verticalScrollBar()->setValue(200);
    s->DefinePrim(SdfPath("/AfterScroll"));
    wait(700);
    CHECK(!panel.isOutOfDate() && shown(panel).contains("AfterScroll"), "auto-refreshed after the edit");
    CHECK(panel.lineView()->verticalScrollBar()->value() == 200, "at the same line: [%d]",
          panel.lineView()->verticalScrollBar()->value());

    // Only what's on screen is coloured.
    const int blocks = panel.lineView()->lineCount();
    const int coloured = panel.lineView()->formattedLines();
    CHECK(coloured > 0 && coloured < blocks / 4, "%d of %d lines coloured", coloured, blocks);
    // The first line on screen with something to colour has its colours.
    int line = panel.lineView()->firstVisibleLine();
    while (line < blocks && !panel.lineView()->lineText(line).contains("def")) ++line;
    CHECK(line < blocks && panel.lineView()->isLaidOut(line), "a def on screen is laid out: %d", line);
    CHECK(!panel.lineView()->isLaidOut(blocks - 1), "the last line, off screen, isn't");

    // Find: the next match from the cursor, and the count.
    panel.showFind();
    auto* find = panel.findChild<QLineEdit*>();
    CHECK(find && find->isVisible(), "Ctrl+F shows the find bar");
    find->setText(QStringLiteral("def \"P1"));
    wait(400);
    CHECK(panel.lineView()->selectedText() == "def \"P1", "the first match selected");
    QString count;
    for (QLabel* label : panel.findChildren<QLabel*>()) {
        if (label->text().endsWith("matches")) count = label->text();
    }
    CHECK(count == "111 matches", "counted: [%s]", qPrintable(count));  // P1, P10-19, P100-199
    QToolButton* close = nullptr;
    for (QToolButton* button : panel.findChildren<QToolButton*>()) {
        if (button->toolTip() == "Close (Esc)") close = button;
    }
    CHECK(close && close->isVisible(), "the find bar has a close button");
    if (close) close->click();
    CHECK(!find->isVisible(), "which closes it");
    QStandardPaths::setTestModeEnabled(false);
}

// docs/USDA_VIEW.md Â§5: a ~100 MB layer, timed. Only with USDCC_BENCH=1.
USDCC_TEST(usda_view_benchmark) {
    if (!std::getenv("USDCC_BENCH")) return;
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    const std::string text = bigLayer(190000);
    StageManager stages;
    const StageRefPtr stage = stages.createStage();
    stage->usdStage()->GetRootLayer()->ImportFromString(text);
    UsdaViewPanel panel(&stages, stage);
    panel.resize(1200, 900);
    panel.show();
    QElapsedTimer timer;
    timer.start();
    panel.refresh();
    QApplication::processEvents();
    const qint64 total = timer.elapsed();
    const auto& t = panel.lastTimings();
    std::printf("usda_view_benchmark: %.1f MB, %d lines: export %lld ms, scan %lld ms, document %lld ms, "
                "to first paint %lld ms\n",
                panel.textBytes() / 1048576.0, panel.lineView()->lineCount(), t.exportMs, t.scanMs, t.documentMs,
                total);
    timer.restart();
    for (int i = 0; i < 50; ++i) {
        panel.lineView()->verticalScrollBar()->setValue(i * panel.lineView()->lineCount() / 50);
        QApplication::processEvents();
    }
    std::printf("usda_view_benchmark: 50 jumps through the file: %lld ms\n", timer.elapsed());
    CHECK(panel.lineView()->formattedLines() < 20000, "coloured only what was shown: %d lines",
          panel.lineView()->formattedLines());

    // A saved text file is read as it is, not exported.
    QTemporaryDir dir;
    const std::string file = dir.filePath(QStringLiteral("big.usda")).toStdString();
    stage->usdStage()->GetRootLayer()->Export(file);
    const StageRefPtr saved = stages.openStage(file);
    UsdaViewPanel filePanel(&stages, saved);
    filePanel.resize(1200, 900);
    filePanel.show();
    timer.restart();
    filePanel.refresh();
    QApplication::processEvents();
    std::printf("usda_view_benchmark: the saved file, %.1f MB: read %lld ms, scan %lld ms, view %lld ms, "
                "to first paint %lld ms\n",
                filePanel.textBytes() / 1048576.0, filePanel.lastTimings().exportMs, filePanel.lastTimings().scanMs,
                filePanel.lastTimings().documentMs, timer.elapsed());
    CHECK(filePanel.isFromFile(), "read from the file");
    timer.restart();
    const int matches = filePanel.lineView()->countMatches(QStringLiteral("Mesh_1"));
    std::printf("usda_view_benchmark: counting %d matches: %lld ms\n", matches, timer.elapsed());
    timer.restart();
    const std::size_t prims = usdcc::usd::usda::primLines(filePanel.lineView()->text()).size();
    std::printf("usda_view_benchmark: the prim index, %zu prims: %lld ms\n", prims, timer.elapsed());
    // What copying the layer costs, so the export could run on a copy off the UI thread.
    timer.restart();
    const SdfLayerRefPtr copy = SdfLayer::CreateAnonymous("copy.usda");
    copy->TransferContent(stage->usdStage()->GetRootLayer());
    std::printf("usda_view_benchmark: copying the layer: %lld ms\n", timer.elapsed());

    // A more usual size.
    stage->usdStage()->GetRootLayer()->ImportFromString(bigLayer(19000));
    timer.restart();
    panel.refresh();
    QApplication::processEvents();
    std::printf("usda_view_benchmark: %.1f MB: export %lld ms, document %lld ms, to first paint %lld ms\n",
                panel.textBytes() / 1048576.0, panel.lastTimings().exportMs, panel.lastTimings().documentMs,
                timer.elapsed());
    QStandardPaths::setTestModeEnabled(false);
}

// A saved text layer is shown as it is in its file (comments and all), an
// edited one as USD writes it (docs/USDA_VIEW.md Â§9.2).
USDCC_TEST(usda_view_reads_saved_files) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    {
        QFile file(dir.filePath(QStringLiteral("shot.usda")));
        if (file.open(QIODevice::WriteOnly)) file.write("#usda 1.0\n# a comment USD would drop\ndef \"World\"\n{\n}\n");
    }
    StageManager stages;
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    if (!stage) return;
    UsdaViewPanel panel(&stages, stage);
    panel.show();
    CHECK(panel.isFromFile() && shown(panel).contains("a comment USD would drop"), "the file as it is");
    stage->usdStage()->DefinePrim(SdfPath("/Edited"));
    panel.refresh();
    CHECK(!panel.isFromFile() && shown(panel).contains("Edited") && !shown(panel).contains("a comment USD would drop"),
          "edited: as USD writes it");
    QStandardPaths::setTestModeEnabled(false);
}

// LineView: positions in UTF-16 columns over UTF-8 text, selection and copy,
// find (case-insensitive, wrapping) and the count.
USDCC_TEST(line_view) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    usdcc::ui::LineView view;
    view.resize(400, 300);
    view.show();
    view.setText("first line\r\nZwei \xC3\xA4pfel, two APPLES\nthird apple\n");
    CHECK(view.lineCount() == 4 && view.lineText(0) == "first line", "lines, without the \\r: [%s]",
          qPrintable(view.lineText(0)));
    CHECK(view.lineText(1) == QString::fromUtf8("Zwei \xC3\xA4pfel, two APPLES"), "UTF-8 decoded");

    view.setSelection({1, 5}, {1, 10});
    CHECK(view.selectedText() == QString::fromUtf8("\xC3\xA4pfel"), "a selection over a non-ASCII letter: [%s]",
          qPrintable(view.selectedText()));
    view.setSelection({0, 6}, {1, 4});
    CHECK(view.selectedText() == "line\r\nZwei", "across lines, as in the text");

    view.setCursorPosition({0, 0});
    CHECK(view.find(QStringLiteral("apple")) && view.cursorPosition().line == 1 && view.selectedText() == "APPLE",
          "find ignores case: [%s]", qPrintable(view.selectedText()));
    CHECK(view.find(QStringLiteral("apple")) && view.cursorPosition().line == 2, "the next one");
    CHECK(view.find(QStringLiteral("apple")) && view.cursorPosition().line == 1, "and wraps around");
    CHECK(view.find(QStringLiteral("apple"), true) && view.cursorPosition().line == 2, "backward wraps too");
    CHECK(!view.find(QStringLiteral("pear")), "no match: nothing found");
    CHECK(view.countMatches(QStringLiteral("APPLE")) == 2 && view.countMatches(QStringLiteral("e")) == 5,
          "counted: [%d]", view.countMatches(QStringLiteral("e")));
    view.selectAll();
    CHECK(view.selectedText().startsWith("first") && view.selectedText().endsWith("apple\n"), "select all");
    QStandardPaths::setTestModeEnabled(false);
}

// Links (docs/USDA_VIEW.md Â§6): an asset path resolved against the layer
// shown, opened here with Back and Forward, in a new panel, a sublayer shown
// as the stack's own; a missing file; a prim path selecting; the link menu;
// Follow Selection.
USDCC_TEST(usda_view_links) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    auto write = [&dir](const char* name, const char* text) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    };
    write("chair.usda", "#usda 1.0\ndef Xform \"Chair\"\n{\n}\n");
    write("sub.usda", "#usda 1.0\ndef \"FromSub\"\n{\n}\n");
    write("shot.usda",
          "#usda 1.0\n(\n    subLayers = [@./sub.usda@]\n)\n\n"
          "def Xform \"World\"\n{\n"
          "    def \"Chair\" (\n        references = @./chair.usda@</Chair>\n    )\n    {\n    }\n"
          "    rel target = </World/Chair>\n"
          "    def \"Missing\" (\n        references = @./missing.usda@\n    )\n    {\n    }\n"
          "}\n");
    StageManager stages;
    const StageRefPtr stage = stages.openStage(dir.filePath(QStringLiteral("shot.usda")).toStdString());
    if (!stage) return;
    UsdaViewPanel panel(&stages, stage);
    panel.resize(700, 220);  // shorter than the text, so it scrolls
    panel.show();
    usdcc::ui::LineView* view = panel.lineView();

    // Where the reference's link is, as the view finds it.
    int line = 0;
    while (line < view->lineCount() && !view->lineText(line).contains("@./chair.usda@")) ++line;
    const int column = int(view->lineText(line).indexOf('@')) + 2;
    usdcc::ui::LineView::Link link;
    CHECK(view->linkAt({line, column}, &link) && link.kind == UsdaViewPanel::AssetLink && link.text == "@./chair.usda@",
          "the asset path is a link: [%s]", qPrintable(link.text));
    bool found = false;
    CHECK(QFileInfo(panel.resolveAsset(QStringLiteral("./chair.usda"), &found)).fileName() == "chair.usda" && found,
          "resolved next to shot.usda");
    panel.resolveAsset(QStringLiteral("./missing.usda"), &found);
    CHECK(!found && !panel.openLink(QStringLiteral("./missing.usda")), "a missing file: not found, not opened");

    // Open it here, then Back and Forward.
    view->verticalScrollBar()->setValue(2);
    CHECK(view->verticalScrollBar()->value() == 2, "scrolled down two lines first");
    Q_EMIT view->linkActivated(link, Qt::LeftButton, Qt::NoModifier);
    wait(50);
    CHECK(panel.source().kind == UsdaViewPanel::Source::Kind::File && shown(panel).contains("def Xform \"Chair\"") &&
              panel.canGoBack(),
          "a click opens the file here");
    panel.back();
    CHECK(panel.source().kind == UsdaViewPanel::Source::Kind::Layer && shown(panel).contains("World") &&
              view->verticalScrollBar()->value() == 2 && panel.canGoForward(),
          "Back: the stage's layer, where it was: [%d]", view->verticalScrollBar()->value());
    panel.forward();
    CHECK(shown(panel).contains("def Xform \"Chair\"") && !panel.canGoForward(), "Forward: the file again");
    panel.back();

    // A sublayer opens as the stack's own layer.
    CHECK(panel.openLink(QStringLiteral("./sub.usda")) && panel.source().kind == UsdaViewPanel::Source::Kind::Layer &&
              shown(panel).contains("FromSub"),
          "a sublayer link shows that layer of the stack");
    panel.back();

    // Ctrl+click: a new panel, through the handler main.cpp sets.
    std::unique_ptr<UsdaViewPanel> made;
    UsdaViewPanel::setNewPanelHandler([&](const StageRefPtr& s) {
        made = std::make_unique<UsdaViewPanel>(&stages, s);
        return made.get();
    });
    Q_EMIT view->linkActivated(link, Qt::LeftButton, Qt::ControlModifier);
    wait(50);
    CHECK(made && made->source().kind == UsdaViewPanel::Source::Kind::File &&
              shown(*made).contains("def Xform \"Chair\"") && panel.source().kind == UsdaViewPanel::Source::Kind::Layer,
          "Ctrl+click opens it in a new panel, this one stays");
    UsdaViewPanel::setNewPanelHandler({});

    // A prim path selects its prim.
    line = 0;
    while (line < view->lineCount() && !view->lineText(line).contains("</World/Chair>")) ++line;
    usdcc::ui::LineView::Link primLink;
    CHECK(view->linkAt({line, int(view->lineText(line).indexOf('<')) + 1}, &primLink) &&
              primLink.kind == UsdaViewPanel::PrimLink,
          "a prim path is a link");
    Q_EMIT view->linkActivated(primLink, Qt::LeftButton, Qt::NoModifier);
    CHECK(stages.selectedPaths(stage) == std::vector<SdfPath>({SdfPath("/World/Chair")}), "and selects the prim");

    // The link menu.
    QMenu menu;
    Q_EMIT view->linkMenuRequested(&menu, link);
    QStringList entries;
    for (QAction* action : menu.actions()) {
        if (!action->isSeparator()) entries << action->text();
    }
    CHECK(entries == QStringList({"Open Here", "Open in New Panel", "Open as Stage", "Reveal in Explorer", "Copy Path",
                                  "Copy Resolved Path"}),
          "the link menu: [%s]", qPrintable(entries.join(", ")));

    // Follow Selection: to the selected prim's def.
    panel.setFollowSelection(true);
    stages.setSelectedPaths(stage, {SdfPath("/World/Missing")});
    CHECK(view->lineText(view->cursorPosition().line).contains("def \"Missing\""), "Follow Selection: at its def: [%s]",
          qPrintable(view->lineText(view->cursorPosition().line)));
    panel.setFollowSelection(false);
    QStandardPaths::setTestModeEnabled(false);
}
