// The asset picker's save mode (docs/ASSET_PICKER.md §5) and the layer saves that use it,
// on disk and on WebDAV servers inside this program (assets_test_server.h): TEST over PROD,
// where a save goes to TEST. Runs in usdcc_ui_tests (offscreen), which registers the WebDAV
// plugin at start (ui_test_main.cpp).
#include "test_harness.h"

#include "assets_test_server.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/transport.h"
#include "usdcc/usd/asset_picker.h"
#include "usdcc/usd/layer_actions.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/stage.h"

#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace usdcc::usd;
namespace assets = usdcc::assets;
namespace fs = std::filesystem;
using usdcc::test::FolderServer;
using usdcc::test::ServerRequest;
using usdcc::test::TestServer;

namespace {

void writeFile(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

bool waitForListing(const AssetPickerDialog& dialog) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!dialog.listing()) break;
    }
    QCoreApplication::processEvents();
    return !dialog.listing();
}

fs::path pathOf(const QString& file) { return fs::path(file.toStdWString()); }

// The dialog sizes itself through UiScaleController::instance(): a controller of the test's
// own, or a lasting one is made that later tests' controllers sit behind (as in
// asset_picker_test.cpp).
struct ScopedScale {
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       dir.filePath(QStringLiteral("ui.ini"))};
};

const char* kShot = "#usda 1.0\n(\n    defaultPrim = \"Shot\"\n)\n\ndef Xform \"Shot\"\n{\n}\n";

// TEST over PROD, as two WebDAV servers over folders; PROD read-only.
struct Servers {
    QTemporaryDir folder;
    FolderServer prodFolder;
    FolderServer testFolder;
    TestServer prod;
    TestServer test;
    assets::Config config;

    Servers()
        : prod([this](const ServerRequest& request) { return serveFolder(prodFolder, request); }),
          test([this](const ServerRequest& request) { return serveFolder(testFolder, request); }) {
        prodFolder.root = pathOf(folder.filePath(QStringLiteral("prod")));
        testFolder.root = pathOf(folder.filePath(QStringLiteral("test")));
        writeFile(prodFolder.root / "shots/shot.usda", kShot);
        fs::create_directories(testFolder.root / "shots");
        assets::DiskCache::setSharedRoot(pathOf(folder.filePath(QStringLiteral("cache"))));
        assets::setLookupLifetime(std::chrono::milliseconds(0));
        assets::Endpoint prodEndpoint;
        prodEndpoint.id = "prod";
        prodEndpoint.scheme = "webdav";
        prodEndpoint.url = prod.url();
        prodEndpoint.readOnly = true;
        assets::Endpoint testEndpoint = prodEndpoint;
        testEndpoint.id = "test";
        testEndpoint.url = test.url();
        testEndpoint.readOnly = false;
        config.environment = "prod";
        config.endpoints["prod"] = prodEndpoint;
        config.endpoints["test"] = testEndpoint;
        config.environments["prod"]["webdav"]["studio"] = {"prod"};
        config.environments["test"]["webdav"]["studio"] = {"test", "prod"};
        assets::setDefaultConfig(config);
    }
    ~Servers() { assets::useDefaultConfigFile(); }

    PXR_NS::ArResolverContext context(const std::string& environment) const {
        return PXR_NS::ArResolverContext(assets::Context(config, environment));
    }
};

bool webdavAvailable() {
    assets::loadProtocolPlugins();
    return assets::transportFor("webdav") != nullptr;
}

}  // namespace

USDCC_TEST(asset_picker_save_on_disk) {
    ScopedScale scale;
    QTemporaryDir folder;
    AssetPickerOptions options;
    options.save = true;
    options.title = QStringLiteral("Save Test");
    options.filters = {QStringLiteral("USD Layers (*.usda *.usd *.usdc)")};
    options.start = folder.filePath(QStringLiteral("layer.usda"));
    AssetPickerDialog dialog(options);
    waitForListing(dialog);
    auto* box = dialog.findChild<QDialogButtonBox*>();
    CHECK(box && box->button(QDialogButtonBox::Save) && !box->button(QDialogButtonBox::Open), "a Save button, not Open");
    CHECK(dialog.findChild<QToolButton*>(QStringLiteral("assetNewFolder")), "and New Folder");
    CHECK(dialog.fileNameEdit()->text() == QStringLiteral("layer.usda") &&
              QDir(dialog.location()) == QDir(folder.path()),
          "a file that isn't there yet starts in its folder, with its name ready");

    dialog.fileNameEdit()->setText(QStringLiteral("shot"));
    dialog.accept();
    CHECK(dialog.QDialog::result() == QDialog::Accepted &&
              QDir::cleanPath(dialog.chosenFile()) == QDir::cleanPath(folder.filePath(QStringLiteral("shot.usda"))),
          "a name without an extension gets the filter's first: %s", dialog.chosenFile().toUtf8().constData());

    writeFile(pathOf(folder.filePath(QStringLiteral("there.usda"))), kShot);
    AssetPickerDialog again(options);
    waitForListing(again);
    int asked = 0;
    bool answer = false;
    again.setReplaceQuestion([&](const QString&) {
        ++asked;
        return answer;
    });
    again.fileNameEdit()->setText(QStringLiteral("there.usda"));
    again.accept();
    CHECK(asked == 1 && again.QDialog::result() != QDialog::Accepted, "a file that's there is asked about, and kept on No");
    answer = true;
    again.accept();
    CHECK(asked == 2 && again.QDialog::result() == QDialog::Accepted, "and replaced on Yes");

    AssetPickerDialog folders(options);
    waitForListing(folders);
    CHECK(folders.newFolder(QStringLiteral("shots")) && QDir(folder.filePath(QStringLiteral("shots"))).exists() &&
              QDir(folders.location()) == QDir(folder.filePath(QStringLiteral("shots"))),
          "New Folder makes the folder on disk and goes there");
}

USDCC_TEST(asset_picker_save_on_a_server) {
    ScopedScale scale;
    if (!webdavAvailable()) {
        CHECK(false, "the WebDAV protocol plugin isn't loaded");
        return;
    }
    Servers servers;
    AssetPickerOptions options;
    options.save = true;
    options.filters = {QStringLiteral("USD Layers (*.usda *.usd *.usdc)")};
    options.start = QStringLiteral("webdav://studio/shots/");
    options.context = servers.context("test");
    {
        AssetPickerDialog dialog(options);
        waitForListing(dialog);
        CHECK(dialog.saveNote() == QStringLiteral("Saves to test"), "the note names the server a save goes to: %s",
              dialog.saveNote().toUtf8().constData());
        int asked = 0;
        dialog.setReplaceQuestion([&](const QString&) {
            ++asked;
            return false;
        });
        dialog.fileNameEdit()->setText(QStringLiteral("shot"));
        dialog.accept();
        CHECK(asked == 0 && dialog.QDialog::result() == QDialog::Accepted &&
                  dialog.chosenFile() == QStringLiteral("webdav://studio/shots/shot.usda"),
              "a file only PROD has isn't replaced by a save, which writes TEST's: no question (%d), %s", asked,
              dialog.chosenFile().toUtf8().constData());
    }
    writeFile(servers.testFolder.root / "shots/mine.usda", kShot);
    {
        AssetPickerDialog dialog(options);
        waitForListing(dialog);
        int asked = 0;
        dialog.setReplaceQuestion([&](const QString&) {
            ++asked;
            return true;
        });
        dialog.fileNameEdit()->setText(QStringLiteral("mine.usda"));
        dialog.accept();
        CHECK(asked == 1 && dialog.QDialog::result() == QDialog::Accepted, "one TEST has is asked about");

        AssetPickerDialog folders(options);
        waitForListing(folders);
        CHECK(folders.newFolder(QStringLiteral("s020")) && folders.location() == QStringLiteral("webdav://studio/shots/s020/"),
              "New Folder on a server goes there: %s", folders.location().toUtf8().constData());
        waitForListing(folders);
        CHECK(folders.statusLabel()->text().contains(QStringLiteral("new folder")),
              "and says it's made when something is saved in it: %s", folders.statusLabel()->text().toUtf8().constData());
    }
    options.context = servers.context("prod");
    {
        AssetPickerDialog dialog(options);
        waitForListing(dialog);
        CHECK(dialog.saveNote().startsWith(QStringLiteral("Can't save here")),
              "where every server is read-only, the note says so: %s", dialog.saveNote().toUtf8().constData());
        dialog.fileNameEdit()->setText(QStringLiteral("other.usda"));
        dialog.accept();
        CHECK(dialog.QDialog::result() != QDialog::Accepted && dialog.statusLabel()->text().startsWith(QStringLiteral("Can't save here")),
              "and nothing is chosen there");
    }
}

USDCC_TEST(asset_picker_save_layers_to_a_server) {
    if (!webdavAvailable()) {
        CHECK(false, "the WebDAV protocol plugin isn't loaded");
        return;
    }
    Servers servers;
    const PXR_NS::UsdStageRefPtr usd = PXR_NS::UsdStage::Open("webdav://studio/shots/shot.usda", servers.context("test"));
    CHECK(usd, "a stage from PROD, in the test environment");
    if (!usd) return;
    StageRefPtr stage = std::make_shared<Stage>(usd);
    usd->DefinePrim(PXR_NS::SdfPath("/Shot/Added"));
    QString error;
    CHECK(layer_actions::saveStageAs(stage, usd->GetRootLayer()->GetIdentifier(), &error),
          "saving it: %s", error.toUtf8().constData());
    CHECK(readFile(servers.testFolder.root / "shots/shot.usda").find("Added") != std::string::npos,
          "writes it to TEST");
    CHECK(readFile(servers.prodFolder.root / "shots/shot.usda").find("Added") == std::string::npos, "PROD untouched");

    CHECK(layer_actions::saveStageAs(stage, "webdav://studio/shots/copy.usda", &error) &&
              fs::exists(servers.testFolder.root / "shots/copy.usda"),
          "Save Stage As to a new file on the server writes it to TEST: %s", error.toUtf8().constData());

    // Someone else changes TEST's file meanwhile: saving over it is a clear error.
    writeFile(servers.testFolder.root / "shots/shot.usda", std::string(kShot) + "\n# changed by someone else\n");
    usd->DefinePrim(PXR_NS::SdfPath("/Shot/Again"));
    const bool saved = layer_actions::saveStageAs(stage, usd->GetRootLayer()->GetIdentifier(), &error);
    CHECK(!saved && error.startsWith(QStringLiteral("Can't write webdav://studio/shots/shot.usda")),
          "saving over a file changed on the server fails, naming it: %s", error.toUtf8().constData());
    CHECK(error.contains(QStringLiteral("changed"), Qt::CaseInsensitive),
          "and says why: %s", error.toUtf8().constData());

    servers.config.environments["test"]["webdav"]["studio"] = {"prod"};
    const PXR_NS::UsdStageRefPtr readOnly =
        PXR_NS::UsdStage::Open("webdav://studio/shots/shot.usda", PXR_NS::ArResolverContext(assets::Context(servers.config, "prod")));
    if (readOnly) {
        StageRefPtr other = std::make_shared<Stage>(readOnly);
        CHECK(!layer_actions::saveStageAs(other, "webdav://studio/shots/third.usda", &error) && !error.isEmpty(),
              "where every server is read-only, Save As fails with a reason: %s", error.toUtf8().constData());
    }
}
