// The USDA view (docs/USDA_VIEW.md): what each Show mode shows, colouring
// only what's on screen, refreshing after an edit (by itself for a small
// layer, by hand for the flattened stage), keeping the scroll position, and
// Find. With USDCC_BENCH=1 it also times a generated ~100 MB layer (Ã‚Â§5).
// Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/line_view.h"
#include "usdcc/usd/prim_tree_dialog.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/usd_prim_tree_model.h"
#include "usdcc/usd/usda_preview_dialog.h"
#include "usdcc/usd/usda_syntax.h"
#include "usdcc/usd/usda_view_panel.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QPushButton>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
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
using usdcc::usd::UsdaView;
using usdcc::usd::UsdaViewPanel;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void wait(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QApplication::processEvents();
}

// Until done() or the time is up: a debounce timer can fire late on a busy
// machine (a CI runner), so a fixed wait would be a race.
bool waitFor(const std::function<bool()>& done, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QApplication::processEvents();
    }
    return true;
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
    waitFor([&] { return panel.isOutOfDate(); });
    wait(700);  // longer than the auto-refresh delay, which mustn't apply here
    CHECK(panel.isOutOfDate() && !shown(panel).contains("Late"), "flattened stage: out of date, not refreshed");
    panel.refresh();
    CHECK(!panel.isOutOfDate() && shown(panel).contains("Late"), "until Refresh");

    // A small layer refreshes itself after an edit, keeping its scroll position.
    panel.setSource(UsdaViewPanel::Source());
    for (int i = 0; i < 300; ++i) s->DefinePrim(SdfPath("/World/P" + std::to_string(i)));
    panel.refresh();
    panel.lineView()->verticalScrollBar()->setValue(200);
    s->DefinePrim(SdfPath("/AfterScroll"));
    CHECK(waitFor([&] { return !panel.isOutOfDate() && shown(panel).contains("AfterScroll"); }),
          "auto-refreshed after the edit");
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
    QString count;
    waitFor([&] {
        for (QLabel* label : panel.findChildren<QLabel*>()) {
            if (label->text().endsWith("matches")) count = label->text();
        }
        return !count.isEmpty();
    });
    CHECK(panel.lineView()->selectedText() == "def \"P1", "the first match selected");
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

// docs/USDA_VIEW.md Ã‚Â§5: a ~100 MB layer, timed. Only with USDCC_BENCH=1.
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

    // Show USDA on a file no stage has open: the text, then the stage behind it.
    const std::string previewFile = dir.filePath(QStringLiteral("preview.usda")).toStdString();
    {
        QFile out(QString::fromStdString(previewFile));
        if (out.open(QIODevice::WriteOnly)) out.write(filePanel.lineView()->text().data(), qint64(filePanel.textBytes()));
    }
    timer.restart();
    usdcc::usd::UsdaPreviewDialog preview(QString::fromStdString(previewFile));
    preview.resize(1200, 900);
    preview.show();
    QApplication::processEvents();
    std::printf("usda_view_benchmark: Show USDA: the text in %lld ms", timer.elapsed());
    while (!preview.isStageLoaded() && timer.elapsed() < 300000) QApplication::processEvents();
    std::printf(", the stage (no payloads) after %lld ms\n", timer.elapsed());
    CHECK(preview.isStageLoaded() && preview.view()->stage(), "the stage loaded");

    // Building the tree runs on the UI thread, so it's what can freeze the app.
    timer.restart();
    {
        usdcc::usd::UsdPrimTreeModel treeModel;
        treeModel.setStage(preview.view()->stage()->usdStage());
        std::printf("usda_view_benchmark: Show Prims: building the tree: %lld ms\n", timer.elapsed());
    }

    // Show Prims on it: the stage (open already, so faster), the tree, a filter.
    timer.restart();
    usdcc::usd::PrimTreeDialog tree(QString::fromStdString(previewFile));
    tree.show();
    while (!tree.isStageLoaded() && timer.elapsed() < 300000) QApplication::processEvents();
    QApplication::processEvents();
    std::printf("usda_view_benchmark: Show Prims: the tree after %lld ms", timer.elapsed());
    timer.restart();
    tree.setFilter(QStringLiteral("Mesh_1"));
    QApplication::processEvents();
    std::printf(", a filter in %lld ms: %s\n", timer.elapsed(), qPrintable(tree.statusLabel()->text()));
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
// edited one as USD writes it (docs/USDA_VIEW.md Ã‚Â§9.2).
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

// Links (docs/USDA_VIEW.md Ã‚Â§6): an asset path resolved against the layer
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
    if (made) made->show();  // as the app docks it; a panel makes its text once shown
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
    CHECK(entries == QStringList({"Open Here", "Open in New Panel", "Open as Stage", "Show USDA", "Show Prims",
                                  "Reveal in Explorer", "Copy Path", "Copy Resolved Path"}),
          "the link menu: [%s]", qPrintable(entries.join(", ")));

    // Follow Selection: to the selected prim's def.
    panel.setFollowSelection(true);
    stages.setSelectedPaths(stage, {SdfPath("/World/Missing")});
    CHECK(view->lineText(view->cursorPosition().line).contains("def \"Missing\""), "Follow Selection: at its def: [%s]",
          qPrintable(view->lineText(view->cursorPosition().line)));
    panel.setFollowSelection(false);
    QStandardPaths::setTestModeEnabled(false);
}

// Show USDA (docs/USDA_VIEW.md Â§10): the text before the stage, a private stage
// the app never hears of, links and prim paths in it, one dialog per file,
// a broken file, payloads, and a layer shared with an open stage.
namespace {
bool waitForStage(const usdcc::usd::UsdaPreviewDialog& dialog) {
    QElapsedTimer timer;
    timer.start();
    while (!dialog.isStageLoaded() && timer.elapsed() < 20000) QApplication::processEvents();
    return dialog.isStageLoaded();
}

QStringList showEntries(usdcc::usd::UsdaView* view) {
    auto* show = view->findChild<QComboBox*>();
    QStringList entries;
    for (int i = 0; show && i < show->count(); ++i) entries << show->itemText(i).trimmed();
    return entries;
}
}  // namespace

USDCC_TEST(usda_preview_dialog) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    auto write = [&dir](const char* name, const char* text) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    };
    write("leg.usda", "#usda 1.0\ndef Cube \"Leg\"\n{\n    def \"LegPart\"\n    {\n    }\n}\n");
    write("heavy.usda",
          "#usda 1.0\n(\n    defaultPrim = \"Heavy\"\n)\ndef Xform \"Heavy\"\n{\n    def \"HeavyPart\"\n    {\n    }\n}\n");
    write("sub.usda", "#usda 1.0\ndef \"FromSub\"\n{\n}\n");
    write("chair.usda",
          "#usda 1.0\n# a comment USD would drop\n(\n    subLayers = [@./sub.usda@]\n)\n\n"
          "def Xform \"Chair\" (\n    references = @./leg.usda@</Leg>\n)\n{\n"
          "    def \"Seat\" (\n        payload = @./heavy.usda@\n    )\n    {\n    }\n"
          "    rel target = </Chair/Seat>\n}\n");
    write("broken.usda", "#usda 1.0\ndef this is not usda {{{\n");
    write("wood.png", "");
    const QString chair = dir.filePath(QStringLiteral("chair.usda"));

    StageManager stages;
    usdcc::usd::UsdaPreviewDialog dialog(chair);
    usdcc::usd::UsdaView* view = dialog.view();
    dialog.show();

    // The text at once: before the stage, from the file as it is.
    CHECK(QString::fromStdString(view->lineView()->text()).contains("a comment USD would drop") && view->isFromFile(),
          "the file's text right away");
    CHECK(waitForStage(dialog), "the stage loads behind it");
    wait(20);

    // Its own stage: Show fills in, and the app knows nothing of it.
    CHECK(view->stage() && !view->stage()->usdStage()->GetRootLayer()->GetIdentifier().empty(), "it has a stage");
    CHECK(showEntries(view).contains("Flattened Stage") && showEntries(view).contains("sub.usda"),
          "Show has the layer stack and the flattened views: [%s]", qPrintable(showEntries(view).join(", ")));
    CHECK(view->source().kind == UsdaView::Source::Kind::Layer, "the file is now its root layer");
    CHECK(stages.stages().empty() && !stages.currentStage(), "StageManager never hears of it");
    CHECK(view->isFromFile() && QString::fromStdString(view->lineView()->text()).contains("a comment USD would drop"),
          "and the text stayed as it was");

    // Everything the panel has: a sublayer, the flattened stage, without payloads.
    UsdaView::Source flat;
    flat.kind = UsdaView::Source::Kind::FlattenedStage;
    view->setSource(flat);
    QString text = QString::fromStdString(view->lineView()->text());
    CHECK(text.contains("FromSub") && text.contains("LegPart") && !text.contains("HeavyPart"),
          "flattened: composed, the payload not loaded: [%s]", qPrintable(text));
    CHECK(view->statusLabel()->text().contains("payloads not loaded"), "and it says so: [%s]",
          qPrintable(view->statusLabel()->text()));
    view->stage()->usdStage()->Load();
    view->refresh();
    CHECK(QString::fromStdString(view->lineView()->text()).contains("HeavyPart") &&
              !view->statusLabel()->text().contains("payloads not loaded"),
          "Load Payloads: in the text, and the note gone");

    // A prim path goes to its definition here; there's no selection to change.
    UsdaView::Source rootLayer;
    view->setSource(rootLayer);
    CHECK(view->selectPrim(QStringLiteral("/Chair/Seat")) &&
              view->lineView()->lineText(view->lineView()->cursorPosition().line).contains("def \"Seat\""),
          "a prim path jumps to its def");
    CHECK(!view->selectPrim(QStringLiteral("/Nowhere")), "one the text doesn't define stays put");

    // A link opens here, with Back; Show USDA from one opens a new dialog.
    CHECK(view->openLink(QStringLiteral("./leg.usda")) &&
              QString::fromStdString(view->lineView()->text()).contains("def Cube \"Leg\"") && view->canGoBack(),
          "a link opens here");
    bool found = false;
    view->resolveAsset(QStringLiteral("./leg.usda"), &found);
    CHECK(found, "resolved");
    view->back();
    CHECK(view->source().kind == UsdaView::Source::Kind::Layer, "Back returns");

    // One dialog per file; a closed one frees everything.
    auto* first = usdcc::usd::showUsdaPreview(dir.filePath(QStringLiteral("sub.usda")), nullptr);
    CHECK(first && first->isVisible(), "Show USDA opens a dialog");
    CHECK(usdcc::usd::showUsdaPreview(dir.filePath(QStringLiteral("sub.usda")), nullptr) == first,
          "the same file brings it forward");
    CHECK(!usdcc::usd::showUsdaPreview(dir.filePath(QStringLiteral("wood.png")), nullptr), "not a texture");
    // Closed while it may still be loading: the thread just drops its result.
    QPointer<usdcc::usd::UsdaPreviewDialog> closing(first);
    if (first) first->close();
    wait(100);
    CHECK(!closing, "closing deletes it");
    CHECK(stages.stages().empty(), "still nothing in StageManager");

    // A file that can't load as a stage: its text stays, the status says why.
    usdcc::usd::UsdaPreviewDialog broken(dir.filePath(QStringLiteral("broken.usda")));
    broken.show();
    CHECK(QString::fromStdString(broken.view()->lineView()->text()).contains("not usda"), "a broken file's text shows");
    CHECK(waitForStage(broken) && !broken.view()->stage(), "no stage");
    CHECK(broken.view()->statusLabel()->text().contains("Couldn't load"), "and it says why: [%s]",
          qPrintable(broken.view()->statusLabel()->text()));

    // Open as a stage of the app too: separate stages, shared layers.
    const StageRefPtr appStage = stages.openStage(chair.toStdString());
    usdcc::usd::UsdaPreviewDialog shared(chair);
    shared.show();
    CHECK(waitForStage(shared) && shared.view()->stage()->usdStage() != appStage->usdStage(), "a stage of its own");
    CHECK(stages.stages().size() == 1 && stages.currentStage() == appStage, "the app's stage stays current");
    appStage->usdStage()->DefinePrim(SdfPath("/EditedInApp"));
    wait(700);
    CHECK(QString::fromStdString(shared.view()->lineView()->text()).contains("EditedInApp"),
          "an edit to the shared layer reaches the preview");
    QStandardPaths::setTestModeEnabled(false);
}

// A real file, timed: USDCC_USDA_FILE=<path>. For looking into a slow one.
USDCC_TEST(usda_view_file_timing) {
    const char* path = std::getenv("USDCC_USDA_FILE");
    if (!path) return;
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    const StageRefPtr stage = stages.openStage(path);
    UsdaViewPanel panel(&stages, stage);
    panel.resize(1200, 900);
    QElapsedTimer timer;
    timer.start();
    panel.show();
    QApplication::processEvents();
    std::printf("usda_view_file_timing: shown in %lld ms, %.1f MB, %d lines: export %lld ms, scan %lld ms, document %lld ms\n",
                timer.elapsed(), panel.textBytes() / 1048576.0, panel.lineView()->lineCount(), panel.lastTimings().exportMs,
                panel.lastTimings().scanMs, panel.lastTimings().documentMs);
    std::fflush(stdout);
    timer.restart();
    for (int i = 0; i < panel.lineView()->lineCount(); i += 4) {
        panel.lineView()->verticalScrollBar()->setValue(i);
        QApplication::processEvents();
    }
    std::printf("usda_view_file_timing: scrolling through it: %lld ms\n", timer.elapsed());
    std::fflush(stdout);
    timer.restart();
    for (int i = 0; i < 10; ++i) {
        panel.lineView()->horizontalScrollBar()->setValue(i * panel.lineView()->horizontalScrollBar()->maximum() / 10);
        QApplication::processEvents();
    }
    std::printf("usda_view_file_timing: 10 jumps along the lines: %lld ms\n", timer.elapsed());
    QStandardPaths::setTestModeEnabled(false);
}

// A line of millions of characters, as USD writes a big array: only the part near
// what's on screen is laid out, and everything still works on the whole line.
USDCC_TEST(line_view_very_long_lines) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    constexpr int kFar = 2000000;
    std::string longLine(kFar, 'x');
    longLine += "NEEDLE";
    longLine += std::string(1000000, 'y');
    std::string text = "first\n" + longLine + "\nlast line\n";
    usdcc::ui::LineView view;
    view.resize(800, 400);
    view.setLinkFinder(
        [](int, const QString& line) {
            QList<usdcc::ui::LineView::Link> links;
            const int at = int(line.indexOf(QStringLiteral("NEEDLE")));
            if (at >= 0) {
                usdcc::ui::LineView::Link link;
                link.start = at;
                link.length = 6;
                links.append(link);
            }
            return links;
        },
        {});
    view.show();
    view.setText(std::move(text));
    QApplication::processEvents();

    const int total = kFar + 6 + 1000000;
    CHECK(view.lineCount() >= 3 && int(view.lineText(1).size()) == total, "the whole line is there");
    CHECK(view.isLaidOut(1), "it's on screen");
    CHECK(view.laidOutCharacters(1) < 10000, "but only part of it is laid out: %d", view.laidOutCharacters(1));
    CHECK(view.laidOutCharacters(0) == 5 && view.laidOutCharacters(2) == 9, "and short lines are whole");

    // Going far along it.
    QScrollBar* bar = view.horizontalScrollBar();
    CHECK(bar->maximum() > kFar, "the scroll bar spans the whole line: %d", bar->maximum());
    view.setCursorPosition({1, kFar + 3});
    QApplication::processEvents();
    CHECK(view.cursorPosition().column == kFar + 3, "the cursor goes far along it");
    const int character = view.fontMetrics().horizontalAdvance(u'9');
    CHECK(bar->value() > (kFar - 500) * character && bar->value() <= (kFar + 3) * character, "and it scrolls there: %d", bar->value());
    CHECK(view.laidOutCharacters(1) < 10000, "with still only part laid out: %d", view.laidOutCharacters(1));

    // Selecting across parts that aren't laid out, and copying.
    view.setSelection({1, kFar - 1}, {1, kFar + 7});
    QApplication::processEvents();
    CHECK(view.selectedText() == QStringLiteral("xNEEDLEy"), "selection is by the whole line: [%s]", qPrintable(view.selectedText()));
    view.setSelection({1, 10}, {1, total});
    QApplication::processEvents();
    CHECK(view.selectedText().size() == total - 10, "even from far outside the part laid out");
    view.setCursorPosition({0, 0});

    // Finding, which looks in the text.
    CHECK(view.find(QStringLiteral("needle")), "find reaches it");
    CHECK(view.cursorPosition().line == 1 && view.cursorPosition().column == kFar + 6, "selecting it: %d", view.cursorPosition().column);
    QApplication::processEvents();
    CHECK(view.selectedText() == QStringLiteral("NEEDLE"), "selected");
    CHECK(bar->value() > (kFar - 500) * character, "and scrolled to");
    CHECK(view.countMatches(QStringLiteral("needle")) == 1, "counted once");

    // A link in the part far along, with its column in the whole line.
    usdcc::ui::LineView::Link link;
    CHECK(view.linkAt({1, kFar + 2}, &link) && link.start == kFar && link.text == QStringLiteral("NEEDLE"),
          "a link far along: %d [%s]", link.start, qPrintable(link.text));
    CHECK(!view.linkAt({1, 5}, &link), "and none at the start");
    CHECK(view.linkAt({1, kFar + 2}, &link), "and back, the part moving each time");

    // Back to the start: the layout follows.
    bar->setValue(0);
    QApplication::processEvents();
    CHECK(view.laidOutCharacters(1) < 10000, "at the start, part of it again");

    // A line just past the limit, with what's not ASCII in it, and clicks by column.
    std::string wide = "\xC3\xA4\xC3\xB6\xC3\xBC";
    wide += std::string(5000, 'z');
    view.setText(wide);
    QApplication::processEvents();
    CHECK(view.lineText(0).size() == 5003, "not ASCII: by character");
    view.setSelection({0, 0}, {0, 3});
    QApplication::processEvents();
    CHECK(view.selectedText() == QString::fromUtf8("\xC3\xA4\xC3\xB6\xC3\xBC"), "and selected as such");

    // End of a long line, where the part can't reach as far as asked.
    view.setCursorPosition({0, 5003});
    QApplication::processEvents();
    CHECK(view.cursorPosition().column == 5003, "the very end");
    QStandardPaths::setTestModeEnabled(false);
}

// docs/USDA_VIEW.md §5.1: a panel makes its text only on screen, so a tab behind
// another costs nothing when its stage changes.
USDCC_TEST(usda_view_waits_while_hidden) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    const StageRefPtr first = stages.createStage();
    first->usdStage()->DefinePrim(SdfPath("/First"));
    const StageRefPtr second = stages.createStage();
    second->usdStage()->DefinePrim(SdfPath("/Second"));
    for (int i = 0; i < 200; ++i) second->usdStage()->DefinePrim(SdfPath("/Second/P" + std::to_string(i)));

    UsdaViewPanel panel(&stages, first);
    auto* view = panel.findChild<UsdaView*>();
    CHECK(view && view->isRefreshPending() && shown(panel).isEmpty(), "never shown: no text made yet");
    panel.resize(700, 300);
    panel.show();
    CHECK(!view->isRefreshPending() && shown(panel).contains("First"), "shown: the text at once");

    // Behind another tab: a new stage waits, and the old text goes.
    panel.hide();
    panel.setStage(second);
    CHECK(view->isRefreshPending() && shown(panel).isEmpty(), "hidden: the new stage waits");
    panel.show();
    CHECK(!view->isRefreshPending() && shown(panel).contains("Second") && !shown(panel).contains("First"),
          "shown again: the new stage's text");

    // A session restored into a hidden panel keeps its place for when it's shown.
    panel.hide();
    panel.setStage(first);
    panel.setStage(second);
    QJsonObject state;
    state.insert(QStringLiteral("kind"), 0);
    state.insert(QStringLiteral("scroll"), 40);
    panel.restoreSessionState(state);
    wait(20);
    panel.show();
    wait(20);
    CHECK(panel.lineView()->verticalScrollBar()->value() == 40, "the session's place: [%d]",
          panel.lineView()->verticalScrollBar()->value());
    QStandardPaths::setTestModeEnabled(false);
}

// docs/USDA_VIEW.md §5.1: converting a big binary file waits for Convert.
USDCC_TEST(usda_view_asks_before_converting) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    QTemporaryDir dir;
    const std::string binary = dir.filePath(QStringLiteral("big.usdc")).toStdString();
    const std::string text = dir.filePath(QStringLiteral("big.usda")).toStdString();
    {
        const SdfLayerRefPtr layer = SdfLayer::CreateNew(binary);
        SdfCreatePrimInLayer(layer, SdfPath("/Statue"))->SetSpecifier(SdfSpecifierDef);
        layer->Save();
        layer->Export(text);
    }
    const std::size_t binaryBytes = std::size_t(QFileInfo(QString::fromStdString(binary)).size());
    CHECK(binaryBytes > 0, "a binary layer on disk");

    StageManager stages;
    const StageRefPtr stage = stages.openStage(binary);
    if (!stage) return;
    UsdaViewPanel panel(&stages, stage);
    auto* view = panel.findChild<UsdaView*>();
    view->setAskBeforeExportBytes(binaryBytes);  // as big as the threshold: it asks
    panel.resize(700, 300);
    panel.show();
    QLabel* askLabel = nullptr;
    for (QLabel* label : panel.findChildren<QLabel*>()) {
        if (label->text().contains("binary file")) askLabel = label;
    }
    CHECK(view->isAwaitingConvert() && shown(panel).isEmpty(), "a big binary root layer: not converted yet");
    CHECK(askLabel && askLabel->isVisible() && askLabel->text().contains("big.usdc"), "the bar says what and how big: [%s]",
          askLabel ? qPrintable(askLabel->text()) : "none");
    CHECK(view->statusLabel()->text().contains("not converted"), "and so does the status: [%s]",
          qPrintable(view->statusLabel()->text()));
    auto* convertButton = panel.findChild<QPushButton*>(QStringLiteral("usdaConvertButton"));
    CHECK(convertButton && convertButton->isVisible(), "with Convert");
    if (convertButton) convertButton->click();
    CHECK(!view->isAwaitingConvert() && shown(panel).startsWith("#usda") && shown(panel).contains("Statue") &&
              askLabel && !askLabel->isVisible(),
          "Convert: the text, the bar gone");

    // Another source asks again: the flattened views start from the same file.
    UsdaView::Source flat;
    flat.kind = UsdaView::Source::Kind::FlattenedStage;
    panel.setSource(flat);
    CHECK(view->isAwaitingConvert() && shown(panel).isEmpty(), "the flattened stage asks too");
    view->convert();
    CHECK(!view->isAwaitingConvert() && shown(panel).contains("Statue"), "and converts when told");

    // A text file is read as it is, however big: no question.
    const StageRefPtr textStage = stages.openStage(text);
    if (!textStage) return;
    view->setAskBeforeExportBytes(1);
    panel.setStage(textStage);
    CHECK(!view->isAwaitingConvert() && panel.isFromFile() && shown(panel).contains("Statue"), "a text layer never asks");
    QStandardPaths::setTestModeEnabled(false);
}
