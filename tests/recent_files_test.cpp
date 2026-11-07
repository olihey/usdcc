// File > Open Recent (docs/SETTINGS.md): the list, its setting, what gets added and
// the menu. Runs in usdcc_ui_tests (Qt's offscreen platform).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/recent_files.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QObject>
#include <QTemporaryDir>

#include <memory>

using usdcc::core::Settings;
using namespace usdcc::usd;

namespace {
void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write("#usda 1.0\n");
}

QAction* entry(QMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action->text().contains(text)) return action;
    }
    return nullptr;
}
}  // namespace

USDCC_TEST(recent_files_setting_and_list) {
    TestSettings settings;
    QObject owner;
    defineRecentFilesSetting(&owner);
    Settings& registry = Settings::instance();
    const auto* definition = registry.definition(kRecentFilesCountKey);
    CHECK(definition && definition->category == "Files" && definition->type == usdcc::core::SettingType::Int, "a setting in the dialog");
    CHECK(registry.get<int>(kRecentFilesCountKey) == 10, "by default 10");
    CHECK(recentFiles().isEmpty(), "nothing at first");

    QTemporaryDir dir;
    const auto path = [&dir](int i) { return dir.filePath(QStringLiteral("file%1.usda").arg(i)); };
    addRecentFile(path(1));
    addRecentFile(path(2));
    CHECK(recentFiles().size() == 2 && recentFiles().first().endsWith("file2.usda"), "newest first");
    addRecentFile(path(1));
    CHECK(recentFiles().size() == 2 && recentFiles().first().endsWith("file1.usda"), "opening one again moves it up, once");
    addRecentFile(QDir::toNativeSeparators(path(1)));
    CHECK(recentFiles().size() == 2, "however its path is spelled");
    addRecentFile(dir.path() + "/sub/../file1.usda");
    CHECK(recentFiles().size() == 2, "even through ..");
    addRecentFile(QString());
    CHECK(recentFiles().size() == 2, "no path, nothing added");

    // It survives: it's read back from the file, not kept in memory.
    CHECK(usdcc::ui::appSettings().value("RecentFiles/paths").toStringList().size() == 2, "stored in usdcc.ini");

    // Only as many as the setting says.
    for (int i = 3; i <= 14; ++i) addRecentFile(path(i));
    CHECK(recentFiles().size() == 10, "the default keeps 10: %d", int(recentFiles().size()));
    CHECK(recentFiles().first().endsWith("file14.usda") && recentFiles().last().endsWith("file5.usda"), "the newest 10");

    CHECK(registry.setValue(kRecentFilesCountKey, 3), "lowering it");
    CHECK(recentFiles().size() == 3, "shows fewer");
    CHECK(usdcc::ui::appSettings().value("RecentFiles/paths").toStringList().size() == 3, "and forgets the rest at once");
    CHECK(registry.setValue(kRecentFilesCountKey, 10), "raising it");
    CHECK(recentFiles().size() == 3, "doesn't bring them back");
    addRecentFile(path(20));
    CHECK(recentFiles().size() == 4, "and there's room again");

    removeRecentFile(path(20));
    CHECK(recentFiles().size() == 3 && recentFiles().first().endsWith("file14.usda"), "removing one");

    CHECK(registry.setValue(kRecentFilesCountKey, 0), "0");
    CHECK(recentFiles().isEmpty(), "keeps none");
    addRecentFile(path(1));
    CHECK(recentFiles().isEmpty() && !usdcc::ui::appSettings().contains("RecentFiles/paths"), "and adds none");
    CHECK(!registry.setValue(kRecentFilesCountKey, -1), "not fewer than none");

    registry.reset(kRecentFilesCountKey);
    addRecentFile(path(1));
    clearRecentFiles();
    CHECK(recentFiles().isEmpty(), "clearing");
}

USDCC_TEST(recent_files_menu) {
    TestSettings settings;
    QObject owner;
    defineRecentFilesSetting(&owner);
    QTemporaryDir dir;

    QMenu menu;
    QString opened;
    const auto open = [&opened](const QString& path) { opened = path; };
    populateRecentFilesMenu(&menu, open);
    CHECK(menu.actions().isEmpty(), "nothing to list");

    const QString a = dir.filePath("a.usda");
    const QString shared1 = dir.filePath("one/same.usda");
    const QString shared2 = dir.filePath("two/same.usda");
    const QString amp = dir.filePath("R&D.usda");
    const QString gone = dir.filePath("gone.usda");
    for (const QString& path : {a, shared1, shared2, amp}) touch(path);
    for (const QString& path : {gone, amp, shared2, shared1, a}) addRecentFile(path);

    populateRecentFilesMenu(&menu, open);
    QAction* first = menu.actions().value(0);
    const QString nativeA = QDir::toNativeSeparators(QDir::cleanPath(a));
    CHECK(first && first->text() == "&1  " + nativeA, "numbered, with the whole path: [%s]", first ? qPrintable(first->text()) : "?");
    first->trigger();
    CHECK(opened == QDir::cleanPath(a), "opens that file: [%s]", qPrintable(opened));

    CHECK(entry(menu, QDir::toNativeSeparators(QDir::cleanPath(shared1))) && entry(menu, QDir::toNativeSeparators(QDir::cleanPath(shared2))),
          "so files of the same name can be told apart");
    QAction* ampersand = entry(menu, "R&&D.usda");
    CHECK(ampersand, "an & in a name is shown, not a mnemonic");

    QAction* missing = entry(menu, "gone.usda");
    CHECK(missing && missing->isEnabled(), "missing files stay selectable without an availability check");
    missing->trigger();
    CHECK(opened == QDir::cleanPath(gone), "the open handler owns missing-file error reporting");

    CHECK(menu.actions().at(menu.actions().size() - 2)->isSeparator(), "a separator");
    QAction* clear = menu.actions().last();
    CHECK(clear->text() == "Clear Recent", "then Clear Recent");
    clear->trigger();
    CHECK(recentFiles().isEmpty(), "which clears it");

    populateRecentFilesMenu(&menu, open);
    CHECK(menu.actions().isEmpty(), "filling it again starts over");

    // Past nine there's no number key to give.
    for (int i = 0; i < 12; ++i) {
        const QString path = dir.filePath(QStringLiteral("n%1.usda").arg(i));
        touch(path);
        addRecentFile(path);
    }
    populateRecentFilesMenu(&menu, open);
    CHECK(menu.actions().at(8)->text().startsWith("&9  ") && menu.actions().at(9)->text() == QDir::toNativeSeparators(QDir::cleanPath(recentFiles().at(9))),
          "numbers end at nine");
}

USDCC_TEST(recent_files_menu_offline_paths) {
    TestSettings settings;
    QObject owner;
    defineRecentFilesSetting(&owner);

    // Seed history directly: none of these paths needs to be reachable, and
    // populating or triggering with a recording handler must not access them.
    QStringList paths{QStringLiteral("//usdcc-offline.invalid/share/scene.usda")};
#ifdef Q_OS_WIN
    paths << QStringLiteral("Z:/usdcc-offline/project.usda");
#else
    paths << QStringLiteral("/mnt/usdcc-offline/project.usda");
#endif
    usdcc::ui::appSettings().setValue("RecentFiles/paths", paths);
    QMenu menu;
    QStringList opened;
    const auto open = [&opened](const QString& path) { opened << path; };
    populateRecentFilesMenu(&menu, open);
    CHECK(menu.actions().size() == paths.size() + 2, "offline paths and Clear Recent are listed");
    CHECK(opened.isEmpty(), "opening the menu does not invoke the open handler");
    for (int i = 0; i < paths.size(); ++i) {
        QAction* action = menu.actions().at(i);
        CHECK(action->isEnabled() && action->toolTip() == QDir::toNativeSeparators(paths.at(i)),
              "an offline path stays selectable with its full path as tooltip");
        action->trigger();
    }
    CHECK(opened == paths, "the selected network and drive/mount paths reach the open handler unchanged");
    CHECK(recentFiles() == paths, "menu construction keeps offline paths in history");

    populateRecentFilesMenu(&menu, open);
    CHECK(menu.actions().size() == paths.size() + 2, "reopening the menu retains offline entries");
    menu.actions().last()->trigger();
    CHECK(recentFiles().isEmpty(), "offline entries can be cleared without opening them");
}

USDCC_TEST(recent_files_from_opened_stages) {
    TestSettings settings;
    QObject owner;
    defineRecentFilesSetting(&owner);
    QTemporaryDir dir;
    const QString file = dir.filePath("opened.usda");
    touch(file);

    StageManager stages;
    const auto connection = rememberOpenedStages(stages);
    const StageRefPtr memory = stages.createStage();
    CHECK(recentFiles().isEmpty(), "a stage in memory has no file to add");

    const StageRefPtr stage = stages.openStage(file.toStdString());
    CHECK(stage, "opened");
    CHECK(recentFiles().size() == 1 && recentFiles().first().endsWith("opened.usda"), "opening a stage adds its file");
    stages.closeStage(stage);
    CHECK(recentFiles().size() == 1, "and closing it keeps it there");
    stages.closeStage(memory);
}
