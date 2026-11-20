// The asset picker (docs/ASSET_PICKER.md): browsing a source and the disk, filters, the
// scheme rules, favourites, recent folders and relative paths, on a fake source registered
// here. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/assets/activity.h"
#include "usdcc/core/settings.h"
#include "usdcc/usd/asset_picker.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/asset_settings.h"
#include "usdcc/usd/asset_sources.h"
#include "usdcc/ui/scale_controller.h"

#include <pxr/usd/sdf/layer.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace usdcc::usd;

namespace {

// pickertest://alpha/ holds a USD file, a texture and a folder; pickertest://broken/ fails;
// pickertest://slow/ takes its time.
class FakeSource final : public AssetSource {
public:
    std::atomic<int> lists{0};

    std::string id() const override { return "test.picker"; }
    std::string label() const override { return "Picker test"; }
    std::vector<std::string> schemes() const override { return {"pickertest"}; }
    std::vector<AssetRoot> roots(const std::string&, const PXR_NS::ArResolverContext&) const override {
        return {{"alpha", "pickertest://alpha/"}, {"slow", "pickertest://slow/"}};
    }
    std::vector<AssetListEntry> list(const std::string& uri, const PXR_NS::ArResolverContext&,
                                     usdcc::assets::Cancel& cancel) override {
        ++lists;
        if (uri == "pickertest://alpha/") {
            return {{"shot.usda", false, 2048, 1700000000.0, "test"},
                    {"wood.png", false, 10, 0, "prod"},
                    {"chars", true, 0, 0, {}}};
        }
        if (uri == "pickertest://alpha/chars/") return {{"hero.usd", false, 1, 0, {}}};
        if (uri == "pickertest://locked/") throw AssetLoginNeeded("not allowed: HTTP 401", "locked-login", "pickertest");
        if (uri == "pickertest://slow/") {
            for (int i = 0; i < 40 && !cancel.cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return {{"late.usda", false, 1, 0, {}}};
        }
        throw std::runtime_error("unreachable: the test says so");
    }
    // pickertest://paged/: two pages, of two entries and one.
    AssetListPage listPage(const std::string& uri, const PXR_NS::ArResolverContext& context, usdcc::assets::Cancel& cancel,
                           const std::string& from) override {
        if (uri != "pickertest://paged/") return AssetSource::listPage(uri, context, cancel, from);
        ++lists;
        AssetListPage page;
        if (from.empty()) {
            page.entries = {{"a.usda", false, 1, 0, {}}, {"b.usda", false, 1, 0, {}}};
            page.next = "page2";
        } else if (from == "page2") {
            page.entries = {{"c.usda", false, 1, 0, {}}};
        }
        return page;
    }
};

std::shared_ptr<FakeSource> fakeSource() {
    static const std::shared_ptr<FakeSource> source = []() {
        auto made = std::make_shared<FakeSource>();
        AssetSourceRegistry::instance().add(made);
        return made;
    }();
    return source;
}

// The dialog sizes itself through UiScaleController::instance(): without a controller of
// the test's own, that makes a lasting one on the application, which later tests'
// controllers would then sit behind (scale_controller's checks fail after these).
struct ScopedScale {
    QTemporaryDir dir;
    usdcc::ui::UiScaleController scale{*static_cast<QApplication*>(QCoreApplication::instance()),
                                       dir.filePath(QStringLiteral("ui.ini"))};
};

bool waitForListing(const AssetPickerDialog& dialog) {
    QElapsedTimer timer;
    timer.start();
    while (dialog.listing() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return !dialog.listing();
}

QStringList names(const QTreeWidget* view) {
    QStringList out;
    for (int i = 0; i < view->topLevelItemCount(); ++i) out << view->topLevelItem(i)->text(0);
    return out;
}

QTreeWidgetItem* findTop(const QTreeWidget* view, const QString& text) {
    for (int i = 0; i < view->topLevelItemCount(); ++i) {
        if (view->topLevelItem(i)->text(0) == text) return view->topLevelItem(i);
    }
    return nullptr;
}

QTreeWidgetItem* findPlace(const QTreeWidget* places, const QString& group, const QString& label) {
    QTreeWidgetItem* heading = findTop(places, group);
    if (!heading) return nullptr;
    for (int i = 0; i < heading->childCount(); ++i) {
        if (heading->child(i)->text(0) == label) return heading->child(i);
    }
    return nullptr;
}

void touch(const QString& file) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly)) return;
    out.write("#usda 1.0\n");
}

}  // namespace

USDCC_TEST(asset_picker_relative_paths_on_a_server) {
    CHECK(relativeAssetPath("webdav://studio/chars/hero.usd", "webdav://studio/shots/s010.usd") == "../chars/hero.usd",
          "up and over on one server: %s", relativeAssetPath("webdav://studio/chars/hero.usd", "webdav://studio/shots/s010.usd").c_str());
    CHECK(relativeAssetPath("webdav://studio/shots/tex/a.png", "webdav://studio/shots/s010.usd") == "./tex/a.png",
          "down from the layer's folder");
    CHECK(relativeAssetPath("webdav://studio/chars/hero.usd", "webdav://studio~7c21/shots/s010.usd") == "../chars/hero.usd",
          "a resolved anchor's stack id is no matter");
    CHECK(relativeAssetPath("webdav://lib/chars/hero.usd", "webdav://studio/shots/s010.usd").empty(), "not across two names");
    CHECK(relativeAssetPath("webdavs://studio/a.usd", "webdav://studio/b.usd").empty(), "nor across schemes");
    CHECK(relativeAssetPath("C:/a.usd", "webdav://studio/b.usd").empty() && relativeAssetPath("webdav://studio/a.usd", "C:/b.usd").empty(),
          "nor between a server and the disk");
}

USDCC_TEST(asset_picker_favourites) {
    TestSettings settings;
    CHECK(assetFavorites().empty(), "none at first");
    QTemporaryDir dir;
    QDir().mkpath(dir.filePath("a/hero"));
    QDir().mkpath(dir.filePath("b/hero"));
    CHECK(addAssetFavorite(dir.filePath("a/hero")), "a folder on disk is added");
    CHECK(!addAssetFavorite(QDir::toNativeSeparators(dir.filePath("a/hero"))), "once, however it's spelled");
    CHECK(addAssetFavorite(dir.filePath("b/hero")), "another of the same name");
    CHECK(addAssetFavorite("webdav://studio~7c21/chars"), "a server's folder");
    std::vector<AssetFavorite> favorites = assetFavorites();
    CHECK(favorites.size() == 3 && favorites[0].label == "hero" && favorites[1].label == "hero (b)",
          "named after the folder, and its parent when the name is taken: %s",
          favorites.size() > 1 ? favorites[1].label.toStdString().c_str() : "");
    CHECK(favorites[2].path == "webdav://studio/chars/" && favorites[2].label == "chars",
          "a server's kept as its logical path, without the stack id: %s", favorites[2].path.toStdString().c_str());

    CHECK(renameAssetFavorite(dir.filePath("b/hero"), "Other hero") && moveAssetFavorite("webdav://studio/chars/", -1),
          "renamed and moved");
    favorites = assetFavorites();
    CHECK(favorites[1].label == "chars" && favorites[2].label == "Other hero", "in the new order");
    CHECK(!moveAssetFavorite(dir.filePath("a/hero"), -1), "not above the first");
    CHECK(removeAssetFavorite(dir.filePath("a/hero")) && assetFavorites().size() == 2, "removed");
    CHECK(isAssetFavorite("webdav://studio/chars") && !isAssetFavorite(dir.filePath("a/hero")), "and asked about");
}

USDCC_TEST(asset_picker_schemes) {
    fakeSource();
    const QStringList schemes = assetPickerSchemes();
    CHECK(!schemes.isEmpty() && schemes.front() == "file", "file:// first: %s", schemes.join(", ").toStdString().c_str());
    CHECK(schemes.contains("pickertest"), "a source's scheme");
    CHECK(assetPickerAvailable(), "with a source, the picker is used");
}

USDCC_TEST(asset_picker_browses_a_source) {
    TestSettings settings;
    ScopedScale scale;
    fakeSource();
    AssetPickerOptions options;
    options.filters = {"USD Files (*.usd *.usda *.usdc *.usdz)"};
    options.start = "pickertest://";
    AssetPickerDialog dialog(options);
    CHECK(dialog.location() == "pickertest://", "it starts where it's told: %s", dialog.location().toStdString().c_str());
    CHECK(names(dialog.entriesView()) == QStringList({"alpha", "slow"}), "a scheme's roots, without asking the source");
    CHECK(dialog.schemeBox()->currentData().toString() == "pickertest", "the dropdown follows");
    CHECK(findPlace(dialog.placesView(), "pickertest://", "alpha"), "and Places lists them");

    dialog.navigate("pickertest://alpha");
    CHECK(dialog.listing() && waitForListing(dialog), "a folder is listed on a worker");
    CHECK(dialog.location() == "pickertest://alpha/", "as a folder: %s", dialog.location().toStdString().c_str());
    CHECK(names(dialog.entriesView()) == QStringList({"chars", "shot.usda"}), "folders first, files filtered: %s",
          names(dialog.entriesView()).join(", ").toStdString().c_str());
    CHECK(!dialog.entriesView()->isColumnHidden(3) && findTop(dialog.entriesView(), "shot.usda")->text(3) == "test",
          "where each came from, when there's more than one server");
    dialog.filterBox()->setCurrentIndex(dialog.filterBox()->count() - 1);
    waitForListing(dialog);
    CHECK(names(dialog.entriesView()).contains("wood.png"), "All Files shows the rest");

    dialog.navigate("pickertest://alpha/chars/");
    waitForListing(dialog);
    dialog.favoriteButton()->click();
    CHECK(isAssetFavorite("pickertest://alpha/chars/"), "the star makes the folder shown a favourite");
    CHECK(findPlace(dialog.placesView(), "Favourites", "chars"), "listed in Places");

    dialog.entriesView()->setCurrentItem(findTop(dialog.entriesView(), "hero.usd"));
    dialog.accept();
    CHECK(dialog.result() == dialog.chosenFile() && dialog.chosenFile() == "pickertest://alpha/chars/hero.usd",
          "a listed file is chosen by its path: %s", dialog.chosenFile().toStdString().c_str());
    CHECK(recentAssetFolders().value(0) == "pickertest://alpha/chars/", "and its folder is recent");
}

// A folder too big for one page shows its first and offers the next (docs/S3_RESOLVER.md §5).
USDCC_TEST(asset_picker_shows_more) {
    TestSettings settings;
    ScopedScale scale;
    fakeSource();
    AssetPickerDialog dialog(AssetPickerOptions{});
    dialog.navigate("pickertest://slow/");
    CHECK(dialog.findChild<QWidget*>(QStringLiteral("assetPickerBusy")), "while a page is coming, the list says so");
    waitForListing(dialog);
    // The tree deletes a row's widget later, as Qt does.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!dialog.findChild<QWidget*>(QStringLiteral("assetPickerBusy")), "and not once it's there");
    dialog.navigate("pickertest://paged/");
    waitForListing(dialog);
    auto* more = dialog.findChild<QPushButton*>(QStringLiteral("assetPickerShowMore"));
    CHECK(names(dialog.entriesView()).contains("a.usda") && !names(dialog.entriesView()).contains("c.usda") && more,
          "the first page is shown, with Show More: %s", names(dialog.entriesView()).join(", ").toStdString().c_str());
    CHECK(dialog.statusLabel()->text().contains("there are more"), "and the status says there's more: %s",
          dialog.statusLabel()->text().toStdString().c_str());
    if (!more) return;
    more->click();
    waitForListing(dialog);
    const QStringList all = names(dialog.entriesView());
    CHECK(all.contains("a.usda") && all.contains("c.usda") && !dialog.findChild<QPushButton*>(QStringLiteral("assetPickerShowMore")),
          "Show More adds the next page, and with none after it, goes: %s", all.join(", ").toStdString().c_str());
}

USDCC_TEST(asset_picker_errors_and_refusals) {
    TestSettings settings;
    ScopedScale scale;
    fakeSource();
    AssetPickerDialog dialog(AssetPickerOptions{});
    dialog.navigate("pickertest://broken/");
    waitForListing(dialog);
    CHECK(dialog.statusLabel()->text().contains("unreachable: the test says so"), "a failing listing says why: %s",
          dialog.statusLabel()->text().toStdString().c_str());

    CHECK(!dialog.findChild<QPushButton*>(QStringLiteral("assetPickerLogIn")), "no Log In for a server that's down");
    dialog.navigate("pickertest://locked/");
    waitForListing(dialog);
    auto* login = dialog.findChild<QPushButton*>(QStringLiteral("assetPickerLogIn"));
    CHECK(login && login->text().contains("locked-login"), "a refusal a login may cure offers Log In, for its credential: %s",
          login ? login->text().toStdString().c_str() : "none");

    const QString before = dialog.location();
    dialog.navigate("nope://somewhere/");
    CHECK(dialog.location() == before && dialog.statusLabel()->text().contains("nope://"),
          "a scheme no resolver handles is refused, by name");

    addAssetFavorite("pickertest://ghost/x/");
    AssetPickerDialog again(AssetPickerOptions{});
    QTreeWidgetItem* ghost = findPlace(again.placesView(), "Favourites", "x");
    CHECK(ghost && ghost->toolTip(0).contains("has no server named 'ghost'"),
          "a favourite on a name the environment lacks says so, unasked: %s", ghost ? ghost->toolTip(0).toStdString().c_str() : "");

    // A slow listing left behind doesn't land in the folder shown since.
    const int listsBefore = fakeSource()->lists;
    again.navigate("pickertest://slow/");
    again.navigate("pickertest://alpha/");
    waitForListing(again);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    QCoreApplication::processEvents();
    CHECK(fakeSource()->lists - listsBefore == 2 && !names(again.entriesView()).contains("late.usda"),
          "the slow one is dropped: %s", names(again.entriesView()).join(", ").toStdString().c_str());
}

USDCC_TEST(asset_picker_disk_and_relative_paths) {
    TestSettings settings;
    ScopedScale scale;
    QTemporaryDir dir;
    touch(dir.filePath("assets/chair.usda"));
    touch(dir.filePath("assets/notes.txt"));
    touch(dir.filePath("shots/s010.usda"));
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(dir.filePath("shots/s010.usda").toStdString());
    CHECK(layer, "a layer to write into");

    AssetPickerOptions options;
    options.filters = {"USD Files (*.usd *.usda *.usdc *.usdz)"};
    options.start = "file:///" + dir.filePath("assets/chair.usda");
    options.anchorLayer = layer;
    AssetPickerDialog dialog(options);
    waitForListing(dialog);
    CHECK(dialog.location() == QDir::cleanPath(dir.filePath("assets")), "a file:/// start is a path, its folder shown: %s",
          dialog.location().toStdString().c_str());
    CHECK(dialog.fileNameEdit()->text() == "chair.usda", "with the file's name ready");
    CHECK(names(dialog.entriesView()) == QStringList({"chair.usda"}), "filtered: %s",
          names(dialog.entriesView()).join(", ").toStdString().c_str());
    CHECK(dialog.schemeBox()->currentData().toString() == "file", "on disk, the dropdown says file://");
    CHECK(dialog.relativeButton() && dialog.relativeButton()->isChecked(), "relative, by default, when it goes into a layer");

    dialog.accept();
    CHECK(dialog.chosenFile() == QDir::cleanPath(dir.filePath("assets/chair.usda")), "chosen by its path");
    CHECK(dialog.result() == "../assets/chair.usda", "written relative to the layer: %s", dialog.result().toStdString().c_str());
    CHECK(assetPathForChoice(dialog.chosenFile(), layer, false) == dialog.chosenFile(), "or absolute when asked");

    AssetPickerDialog second(options);
    waitForListing(second);
    second.fileNameEdit()->setText("missing.usda");
    second.accept();
    CHECK(second.chosenFile().isEmpty() && second.statusLabel()->text().contains("Not found"), "a file that isn't there isn't chosen");
    second.navigate(QString());
    CHECK(second.location().isEmpty() && names(second.entriesView()).contains("Home"), "This PC: the drives and home");
}

// Settings > Assets > Allowed buckets (docs/S3_RESOLVER.md §3.2, §7) reaches the
// library's bucket list, and a refused bucket is told once to the window's note.
USDCC_TEST(asset_settings_allowed_buckets) {
    usdcc::core::Settings settings;
    usdcc::usd::defineAssetSettings(settings);
    CHECK(settings.definition(usdcc::usd::kAssetS3BucketsKey), "Settings > Assets has Allowed buckets");
    CHECK(!usdcc::assets::bucketAllowed("studio-assets"), "an unlisted bucket isn't allowed");
    settings.setValue(usdcc::usd::kAssetS3BucketsKey, std::string("studio-*; render-cache"));
    CHECK(usdcc::assets::bucketAllowed("studio-assets") && usdcc::assets::bucketAllowed("render-cache") &&
              !usdcc::assets::bucketAllowed("elsewhere"),
          "the setting's names and patterns are");
    std::vector<std::string> refused;
    usdcc::assets::setBucketRefusedCallback([&refused](const std::string& bucket) { refused.push_back(bucket); });
    usdcc::assets::bucketAllowed("elsewhere");
    usdcc::assets::bucketAllowed("studio-assets");
    usdcc::assets::setBucketRefusedCallback({});
    CHECK(refused == std::vector<std::string>{"elsewhere"}, "a refusal is told, an allowed bucket isn't");
    settings.setValue(usdcc::usd::kAssetS3BucketsKey, std::string());
    CHECK(!usdcc::assets::bucketAllowed("studio-assets"), "and emptied, none are");
}
