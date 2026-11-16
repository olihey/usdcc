// Help > USD Runtime (docs/USD_RUNTIME.md §2): the tabs show what was
// collected, the filters hide rows, a Debug Flags checkbox turns its TF_DEBUG
// flag on at once, and Copy puts the text on the clipboard. Runs in
// usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/usd_runtime_dialog.h"
#include "usdcc/usd/usd_runtime_info.h"

#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <algorithm>

using usdcc::usd::UsdRuntimeDialog;

namespace {
QTreeWidgetItem* findRow(QTreeWidget* tree, const QString& text) {
    const auto found = tree->findItems(text, Qt::MatchExactly, 0);
    return found.isEmpty() ? nullptr : found.front();
}

bool flagEnabled(const std::string& name) {
    const auto flags = usdcc::usd::usdDebugFlags();
    const auto flag = std::find_if(flags.begin(), flags.end(), [&](const auto& f) { return f.name == name; });
    return flag != flags.end() && flag->enabled;
}
}  // namespace

USDCC_TEST(usd_runtime_dialog) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));

    UsdRuntimeDialog dialog;
    const auto& info = dialog.info();
    QStringList tabs;
    for (int i = 0; i < dialog.tabs()->count(); ++i) tabs << dialog.tabs()->tabText(i);
    CHECK(tabs == QStringList({"Overview", "Plugins", "File Formats", "Render Delegates", "Environment", "Debug Flags"}),
          "its tabs: %s", qPrintable(tabs.join(", ")));

    QTreeWidget* overview = dialog.tree("overview");
    CHECK(overview && overview->topLevelItemCount() == static_cast<int>(info.overview.size()) &&
              overview->topLevelItem(0)->text(0) == "OpenUSD" && overview->topLevelItem(0)->child(0)->text(0) == "Version",
          "the overview, a section each, OpenUSD's version first");

    QTreeWidget* plugins = dialog.tree("plugins");
    CHECK(plugins && plugins->topLevelItemCount() == static_cast<int>(info.plugins.size()) && findRow(plugins, "usd"),
          "a row per plugin");
    auto* filter = plugins->parentWidget()->findChild<QLineEdit*>();
    filter->setText("usdgeom");
    int shown = 0;
    bool onlyMatches = true;
    for (int i = 0; i < plugins->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = plugins->topLevelItem(i);
        if (item->isHidden()) continue;
        ++shown;
        onlyMatches = onlyMatches && (item->text(0).contains("usdGeom", Qt::CaseInsensitive) ||
                                      item->text(3).contains("usdGeom", Qt::CaseInsensitive));
    }
    CHECK(shown > 0 && shown < plugins->topLevelItemCount() && onlyMatches, "the filter shows only what matches (%d)", shown);
    filter->clear();

    QTreeWidget* formats = dialog.tree("fileFormats");
    QTreeWidgetItem* usda = formats ? findRow(formats, "usda") : nullptr;
    CHECK(usda && usda->text(1) == "usda" && usda->text(3) == "read, write" && !usda->text(4).isEmpty(),
          "usda among the file formats");
    QTreeWidget* renderers = dialog.tree("renderers");
    CHECK(renderers && findRow(renderers, "GL"), "Storm among the render delegates");
    CHECK(dialog.tree("environment") && dialog.findChild<QPushButton*>("usdRuntimeSettings"),
          "the environment, with a way to Settings > USD");

    QTreeWidget* flags = dialog.tree("debugFlags");
    QTreeWidgetItem* flag = flags ? findRow(flags, "AR_RESOLVER_INIT") : nullptr;
    CHECK(flag && flag->flags().testFlag(Qt::ItemIsUserCheckable) && !flag->text(1).isEmpty(),
          "the debug flags, with checkboxes and descriptions");
    if (flag) {
        const bool was = flagEnabled("AR_RESOLVER_INIT");
        flag->setCheckState(0, was ? Qt::Unchecked : Qt::Checked);
        CHECK(flagEnabled("AR_RESOLVER_INIT") == !was, "a checkbox sets its flag at once");
        flag->setCheckState(0, was ? Qt::Checked : Qt::Unchecked);
        CHECK(flagEnabled("AR_RESOLVER_INIT") == was, "and back");
    }

    dialog.copy();
    CHECK(QGuiApplication::clipboard()->text() == dialog.text() && dialog.text().startsWith("OpenUSD\n  Version: "),
          "Copy puts the text on the clipboard");

    dialog.refresh();
    CHECK(dialog.tree("plugins")->topLevelItemCount() == static_cast<int>(dialog.info().plugins.size()),
          "Refresh fills the tabs again");
}
