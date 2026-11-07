#include "usdcc/usd/recent_files.h"

#include "usdcc/core/settings.h"
#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QObject>
#include <QSettings>

#include <algorithm>

namespace usdcc::usd {

namespace {
constexpr auto kListKey = "RecentFiles/paths";
constexpr int kDefaultCount = 10;

int maximumCount() { return std::max(0, usdcc::core::Settings::instance().get<int>(kRecentFilesCountKey, kDefaultCount)); }

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

QString normalized(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

QStringList storedList() { return usdcc::ui::appSettings().value(kListKey).toStringList(); }

void store(QStringList paths) {
    while (paths.size() > maximumCount()) paths.removeLast();
    QSettings settings = usdcc::ui::appSettings();
    if (paths.isEmpty()) {
        settings.remove(kListKey);
    } else {
        settings.setValue(kListKey, paths);
    }
}

void removeFrom(QStringList& paths, const QString& path) {
    paths.erase(std::remove_if(paths.begin(), paths.end(),
                               [&path](const QString& other) { return other.compare(path, kPathCase) == 0; }),
                paths.end());
}
}  // namespace

void defineRecentFilesSetting(QObject* owner) {
    usdcc::core::Settings::instance().define(
        {.key = kRecentFilesCountKey,
         .category = "Files",
         .group = "Recent Files",
         .label = "Number of recent files",
         .description = "How many files File > Open Recent lists. 0 keeps none.",
         .type = usdcc::core::SettingType::Int,
         .defaultValue = kDefaultCount,
         .minimum = 0,
         .maximum = 100});
    usdcc::ui::onSettingChanged(owner, kRecentFilesCountKey, []() { store(storedList()); });
}

QStringList recentFiles() {
    QStringList paths = storedList();
    while (paths.size() > maximumCount()) paths.removeLast();
    return paths;
}

void addRecentFile(const QString& path) {
    if (path.isEmpty() || maximumCount() == 0) return;
    const QString clean = normalized(path);
    QStringList paths = storedList();
    removeFrom(paths, clean);
    paths.prepend(clean);
    store(paths);
}

void removeRecentFile(const QString& path) {
    QStringList paths = storedList();
    removeFrom(paths, normalized(path));
    store(paths);
}

void clearRecentFiles() { store({}); }

void populateRecentFilesMenu(QMenu* menu, const std::function<void(const QString& path)>& open) {
    menu->clear();
    menu->setToolTipsVisible(true);
    const QStringList paths = recentFiles();
    int number = 0;
    for (const QString& path : paths) {
        const QString nativePath = QDir::toNativeSeparators(path);
        QString label = nativePath;
        label.replace(QLatin1Char('&'), QLatin1String("&&"));  // not a mnemonic
        ++number;
        // Only the first nine can have a number key.
        const QString text = number <= 9 ? QStringLiteral("&%1  %2").arg(number).arg(label) : label;
        // Menu construction must not query the filesystem: an offline UNC
        // share, mapped drive, or network mount can block for minutes. Let the
        // open handler load the chosen path and report any failure instead.
        QAction* action = menu->addAction(text);
        action->setToolTip(nativePath);
        QObject::connect(action, &QAction::triggered, menu, [open, path]() { open(path); });
    }
    if (!paths.isEmpty()) {
        menu->addSeparator();
        menu->addAction(QObject::tr("Clear Recent"), menu, []() { clearRecentFiles(); });
    }
}

usdcc::core::Signal<StageRefPtr>::Connection rememberOpenedStages(StageManager& stages) {
    return stages.stageOpened.connect([](const StageRefPtr& stage) {
        addRecentFile(QString::fromStdString(stage->usdStage()->GetRootLayer()->GetRealPath()));
    });
}

}  // namespace usdcc::usd
