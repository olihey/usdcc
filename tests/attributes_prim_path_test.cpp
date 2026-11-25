// The prim's path at the top of the Attribute Editor is shortened in the middle to
// fit the panel. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/attributes_view_panel.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>

#include <memory>

using usdcc::usd::AttributesViewPanel;
using usdcc::usd::StageManager;
using usdcc::usd::StageRefPtr;
using usdcc::usd::UndoManager;
PXR_NAMESPACE_USING_DIRECTIVE

USDCC_TEST(attributes_prim_path_fits) {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    StageManager stages;
    auto undo = std::make_shared<UndoManager>();
    const StageRefPtr stage = stages.createStage();
    const SdfPath path("/scene/Meshes/Sketchfab_model/IP12PRO_fbx/RootNode/iPhone__12_Pro/iPhone12_Pro_Body_0");
    stage->usdStage()->DefinePrim(path, TfToken("Mesh"));
    const QString full = QString::fromStdString(path.GetString());

    AttributesViewPanel panel(&stages, stage, undo, QStringLiteral("Attributes"));
    panel.resize(2000, 600);
    panel.show();
    stages.setSelectedPaths(stage, {path});
    const auto settle = []() {
        for (int i = 0; i < 8; ++i) QApplication::processEvents();
    };
    settle();

    QLabel* label = panel.findChild<QLabel*>(QStringLiteral("attributesPrimName"));
    CHECK(label, "the path's label");
    if (!label) return;
    const auto shown = [label]() {
        QTextDocument document;
        document.setHtml(label->text());
        return document.toPlainText();
    };
    const QChar ellipsis(0x2026);
    CHECK(shown().startsWith(full) && !shown().contains(ellipsis), "wide: all of it: %s", qPrintable(shown()));

    const int minimumWidth = panel.minimumSizeHint().width();
    for (const int width : {600, 450}) {
        panel.resize(width, 600);
        settle();
        CHECK(panel.width() == width, "the panel gets that narrow: %d, not %d", panel.width(), width);
        const QString text = shown();
        CHECK(text.contains(ellipsis) && !text.startsWith(ellipsis), "its middle is gone: %s", qPrintable(text));
        CHECK(text.startsWith(QStringLiteral("/scene")) && text.contains(QStringLiteral("Body_0")),
              "its ends are there: %s", qPrintable(text));
        CHECK(text.endsWith(QStringLiteral("Mesh")), "and the type: %s", qPrintable(text));
        CHECK(label->sizeHint().width() <= label->width(), "it fits: %d in %d", label->sizeHint().width(), label->width());
        CHECK(label->toolTip() == full, "the tooltip has all of it");
    }
    CHECK(panel.minimumSizeHint().width() == minimumWidth, "the path doesn't set the panel's minimum width");

    panel.resize(2000, 600);
    settle();
    CHECK(shown().startsWith(full), "wide again: all of it again: %s", qPrintable(shown()));
}
