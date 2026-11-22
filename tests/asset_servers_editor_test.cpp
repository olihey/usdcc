// The Asset Servers editor (docs/REMOTE_ASSETS.md §10.1): the dialog over a configuration
// file in a temporary folder, with a transport registered here for its Test button. The
// editing rules themselves are tested in usdcc_assets_tests (assets_test.cpp), and Test
// against a real WebDAV server in assets_net_test.cpp. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/config_editor.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/transport.h"
#include "usdcc/usd/asset_servers_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace usdcc::usd;
namespace assets = usdcc::assets;

namespace {

const char* kConfig = R"({
    "environment": "prod",
    "laterVersion": { "keep": "me" },
    "endpoints": {
        "prod": { "scheme": "edittest", "url": "https://prod.example", "credential": "studio", "readOnly": true },
        "test": { "scheme": "edittest", "url": "https://test.example" },
        "bucket": { "scheme": "s3", "url": "https://minio.example/assets", "region": "eu-central-1" }
    },
    "environments": {
        "prod": { "edittest": { "studio": ["prod"] } },
        "test": { "edittest": { "studio": ["test", "prod"] } }
    }
})";

// Answers for a root listing: three entries from the prod URL, nothing from anywhere else.
class EditTestTransport : public assets::Transport {
public:
    assets::Result<assets::Entry> stat(const assets::Endpoint&, const std::string&, assets::Cancel&) override {
        return assets::Result<assets::Entry>::failure(assets::Status::Missing);
    }
    assets::Result<assets::EntryPage> listPage(const assets::Endpoint& endpoint, const std::string&, const std::string&,
                                               assets::Cancel&) override {
        if (endpoint.url != "https://prod.example") {
            return assets::Result<assets::EntryPage>::failure(assets::Status::Unreachable, "no such host");
        }
        assets::EntryPage page;
        for (const char* name : {"chars", "shots", "lib"}) page.entries.push_back({name, true});
        return assets::Result<assets::EntryPage>::success(page);
    }
    bool canList() const override { return true; }
    assets::Result<assets::Entry> fetch(const assets::Endpoint&, const std::string&, const std::filesystem::path&,
                                        const std::string&, assets::Progress&, assets::Cancel&) override {
        return assets::Result<assets::Entry>::failure(assets::Status::Missing);
    }
};

std::string readFile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void writeFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

bool waitFor(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

std::filesystem::path pathOf(const QString& file) { return std::filesystem::path(file.toStdWString()); }

}  // namespace

USDCC_TEST(asset_servers_editor_edits_and_saves) {
    assets::registerTransport("edittest", std::make_shared<EditTestTransport>());
    QTemporaryDir folder;
    const std::filesystem::path file = pathOf(folder.filePath(QStringLiteral("assets.json")));
    writeFile(file, kConfig);
    qputenv("USDCC_ASSETS_CONFIG", QByteArray(folder.filePath(QStringLiteral("assets.json")).toUtf8()));
    assets::useDefaultConfigFile();
    const assets::Context before(assets::defaultConfig(), "test");

    AssetServersDialog dialog(nullptr, file);
    CHECK(!dialog.readOnly() && dialog.problems().isEmpty(), "a file it can write, with nothing wrong");
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("assetServersTable"));
    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("assetServersSave"));
    CHECK(table && table->rowCount() == 3, "a row per server");
    CHECK(save && !save->isEnabled(), "Save is off until something changes");

    // The S3 fields show for an S3 server only, and edit its options.
    dialog.selectServer(QStringLiteral("bucket"));
    auto* region = dialog.findChild<QLineEdit*>(QStringLiteral("assetServersS3Region"));
    auto* inHost = dialog.findChild<QCheckBox*>(QStringLiteral("assetServersS3BucketInHost"));
    CHECK(region && region->isVisibleTo(&dialog) && region->text() == QStringLiteral("eu-central-1"),
          "an S3 server shows its region");
    inHost->setChecked(true);
    CHECK(dialog.editor().config().endpoints.at("bucket").options.at("bucketInHost") == "true",
          "Bucket in the host name is its option");
    dialog.selectServer(QStringLiteral("test"));
    CHECK(!region->isVisibleTo(&dialog), "and a WebDAV server doesn't show them");

    // Reordering a stack by dragging: the list's rows move, the stack follows.
    dialog.selectEnvironment(QStringLiteral("test"));
    dialog.selectName(QStringLiteral("edittest"), QStringLiteral("studio"));
    auto* stack = dialog.findChild<QListWidget*>(QStringLiteral("assetServersStack"));
    CHECK(stack && stack->count() == 2 && stack->item(0)->text() == QStringLiteral("test"), "TEST over PROD");
    stack->model()->moveRow(QModelIndex(), 1, QModelIndex(), 0);
    CHECK(dialog.editor().config().environments.at("test").at("edittest").at("studio") ==
              std::vector<std::string>({"prod", "test"}),
          "a dragged row reorders the stack");
    dialog.applyStackOrder();

    // A problem shows, and keeps Save off.
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->text() == QStringLiteral("test")) table->item(row, 2)->setText(QString());
    }
    CHECK(!dialog.problems().isEmpty() && dialog.problems().join(QString()).contains(QStringLiteral("'test'")),
          "a server without a URL is a problem, named: %s", dialog.problems().join(QStringLiteral(" / ")).toUtf8().constData());
    CHECK(!save->isEnabled(), "and Save is off while there is one");
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->text() == QStringLiteral("test")) {
            table->item(row, 2)->setText(QStringLiteral("https://test2.example"));
        }
    }
    CHECK(dialog.problems().isEmpty() && save->isEnabled(), "fixed, Save is on");

    CHECK(dialog.save(), "it saves");
    const std::string written = readFile(file);
    CHECK(written.find("https://test2.example") != std::string::npos && written.find("laterVersion") != std::string::npos,
          "the file has the change, and the key this version doesn't know");
    const assets::Result<assets::Config> reread = assets::Config::parse(written);
    CHECK(reread.ok() && reread.value.environments.at("test").at("edittest").at("studio") ==
                             std::vector<std::string>({"prod", "test"}),
          "and the stack in its new order");
    CHECK(assets::defaultConfig().endpoints.at("test").url == "https://test2.example",
          "the library's configuration is read again at once");
    const std::shared_ptr<const assets::Stack> kept = before.stack("edittest", "studio");
    CHECK(kept && kept->endpoints.front().url == "https://test.example",
          "a context made before (an open stage's) keeps the servers it was made with");

    // Test, on a worker: the table's values, unsaved.
    dialog.testServer(QStringLiteral("prod"));
    CHECK(waitFor([&dialog]() { return dialog.testResult().contains(QStringLiteral("answered")); }),
          "Test says a server answered: %s", dialog.testResult().toUtf8().constData());
    CHECK(dialog.testResult().contains(QStringLiteral("3 entries")), "with its root's entries");
    dialog.testServer(QStringLiteral("test"));
    CHECK(waitFor([&dialog]() { return dialog.testResult().contains(QStringLiteral("unreachable")); }),
          "and that one didn't, naming its URL: %s", dialog.testResult().toUtf8().constData());
    CHECK(dialog.testResult().contains(QStringLiteral("https://test2.example")), "the URL as the table has it");

    qunsetenv("USDCC_ASSETS_CONFIG");
    assets::useDefaultConfigFile();
}

USDCC_TEST(asset_servers_editor_shows_a_file_it_cant_write) {
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("assets.json"));
    writeFile(pathOf(path), kConfig);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser);
    {
        AssetServersDialog dialog(nullptr, pathOf(path));
        CHECK(dialog.readOnly(), "a file it can't write opens read-only");
        auto* save = dialog.findChild<QPushButton*>(QStringLiteral("assetServersSave"));
        auto* copy = dialog.findChild<QPushButton*>(QStringLiteral("assetServersSaveCopy"));
        auto* notice = dialog.findChild<QLabel*>(QStringLiteral("assetServersNotice"));
        CHECK(save && !save->isEnabled() && copy && !copy->isHidden(), "Save off, Save a Copy as Mine there");
        CHECK(notice && notice->text().contains(QStringLiteral("can't be written")), "and it says why");
        const std::filesystem::path mine = pathOf(folder.filePath(QStringLiteral("mine/assets.json")));
        CHECK(dialog.saveCopyAsMine(mine) && std::filesystem::exists(mine), "a copy becomes this user's own");
        CHECK(!dialog.readOnly() && dialog.editor().file() == mine, "and is what it edits from then on");
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    assets::useDefaultConfigFile();
}
