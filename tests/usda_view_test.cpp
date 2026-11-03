// The USDA view (docs/USDA_VIEW.md): what each Show mode shows, colouring
// only what's on screen, refreshing after an edit (by itself for a small
// layer, by hand for the flattened stage), keeping the scroll position, and
// Find. With USDCC_BENCH=1 it also times a generated ~100 MB layer (§5).
// Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usda_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QFile>
#include <QLineEdit>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>

#include <cstdio>
#include <cstdlib>
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

// docs/USDA_VIEW.md §5: a ~100 MB layer, timed. Only with USDCC_BENCH=1.
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
// edited one as USD writes it (docs/USDA_VIEW.md §9.2).
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
