#include "test_harness.h"

#include <pxr/base/plug/registry.h>

#include "usdcc/ui/settings.h"

#include <QApplication>
#include <QDir>
#include <QStandardPaths>

int main(int argc, char** argv) {
    // Widgets need a full QApplication; CTest runs this on Qt's offscreen
    // platform (see CMakeLists.txt), so no window ever appears.
    QApplication app(argc, argv);
    // Test mode's settings folder is named after the application: one per process, or two
    // runs at once (another checkout's, CTest's -j) change each other's settings.
    const QString name = QStringLiteral("usdcc_ui_tests-%1").arg(QCoreApplication::applicationPid());
    app.setApplicationName(name);
#ifdef USDCC_UI_TESTS_WEBDAV_PROTOCOL_DIR
    // Before anything asks for the resolver, which finds its URI resolvers once: webdav://
    // for the asset picker's save tests (asset_picker_save_test.cpp).
    PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(USDCC_UI_TESTS_WEBDAV_PROTOCOL_DIR);
#endif
    const int result = usdcc::test::runTests(argc, argv);
    // What the tests left in those folders, in test mode and out of it.
    for (const bool testMode : {true, false}) {
        QStandardPaths::setTestModeEnabled(testMode);
        for (const QString& folder : {usdcc::ui::userConfigFolder(), usdcc::ui::userDataFolder(),
                                      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
                                      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}) {
            if (folder.contains(name)) QDir(folder).removeRecursively();
        }
    }
    return result;
}
