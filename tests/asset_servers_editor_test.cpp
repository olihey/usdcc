// The Asset Servers editor (docs/REMOTE_ASSETS.md §10.1): the dialog over a configuration
// file in a temporary folder, with a transport registered here for its Test button. The
// editing rules themselves are tested in usdcc_assets_tests (assets_test.cpp), and Test
// against a real WebDAV server in assets_net_test.cpp. Runs in usdcc_ui_tests (offscreen).
#include "test_harness.h"
#include "assets_memory_store.h"

#include "usdcc/assets/config.h"
#include "usdcc/assets/config_editor.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/transport.h"
#include "usdcc/usd/asset_servers_editor.h"
#include "usdcc/usd/asset_settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QRadioButton>
#include <QTimer>
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
        if (endpoint.url == "https://locked.example") {
            return assets::Result<assets::EntryPage>::failure(assets::Status::NotAllowed, "401");
        }
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
    ScopedMemoryStore store;
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
    ScopedMemoryStore store;
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

namespace {

QDialogButtonBox* buttonsOf(QWidget* form) { return form->findChild<QDialogButtonBox*>(); }

// Runs the login form, with `drive` in it once it's open. It's closed afterwards if still up.
bool withLoginForm(const QString& credential, const QString& scheme, const std::function<void(QWidget*)>& drive) {
    QTimer::singleShot(0, [&drive]() {
        QWidget* form = QApplication::activeModalWidget();
        if (!form) return;
        drive(form);
        if (auto* dialog = qobject_cast<QDialog*>(form); dialog && dialog->isVisible()) dialog->reject();
    });
    return showAssetLoginDialog(nullptr, credential, scheme);
}

int rowOf(QTableWidget* table, const QString& text) {
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->text() == text) return row;
    }
    return -1;
}

}  // namespace

USDCC_TEST(asset_login_form_offers_saved_logins_and_keeps_passwords) {
    ScopedMemoryStore store;
    assets::setDefaultConfig(assets::Config::parse(kConfig).value);
    store->logins["test"] = {"ada", "secret"};
    store->logins["tok"] = {"", "", "t0ken"};

    withLoginForm(QString(), QString(), [](QWidget* form) {
        auto* id = form->findChild<QComboBox*>(QStringLiteral("credentialId"));
        CHECK(id->findText(QStringLiteral("test")) >= 0, "a saved login the configuration doesn't name is offered");
        CHECK(id->findText(QStringLiteral("studio")) >= 0, "and one it names that isn't saved");
        CHECK(id->currentText().isEmpty(), "a new one is in none's place");
        buttonsOf(form)->button(QDialogButtonBox::Save)->click();
        CHECK(form->isVisible(), "no id isn't saved");
    });
    const bool saved = withLoginForm(QStringLiteral("test"), QString(), [](QWidget* form) {
        auto* secret = form->findChild<QLineEdit*>(QStringLiteral("credentialSecret"));
        CHECK(secret->text().isEmpty() && !secret->placeholderText().isEmpty(), "the password isn't shown, but there");
        form->findChild<QLineEdit*>(QStringLiteral("credentialUser"))->setText(QStringLiteral("eve"));
        buttonsOf(form)->button(QDialogButtonBox::Save)->click();
    });
    CHECK(saved && store->logins["test"].user == "eve" && store->logins["test"].password == "secret",
          "an empty password keeps the saved one");
    withLoginForm(QStringLiteral("tok"), QString(),
                  [](QWidget* form) { buttonsOf(form)->button(QDialogButtonBox::Save)->click(); });
    CHECK(store->logins["tok"].token == "t0ken", "an empty token too");
    CHECK(!withLoginForm(QStringLiteral("fresh"), QString(),
                         [](QWidget* form) {
                             form->findChild<QLineEdit*>(QStringLiteral("credentialUser"))->setText(QStringLiteral("x"));
                             buttonsOf(form)->button(QDialogButtonBox::Save)->click();
                             CHECK(!form->findChild<QLabel*>(QStringLiteral("credentialProblem"))->text().isEmpty(),
                                   "a new login without a password says so");
                         }) &&
              !store->logins.count("fresh"),
          "and isn't saved");
    assets::useDefaultConfigFile();
}

USDCC_TEST(asset_servers_editor_lists_logins) {
    ScopedMemoryStore store;
    assets::registerTransport("edittest", std::make_shared<EditTestTransport>());
    store->logins["test"] = {"ada", "secret"};
    QTemporaryDir folder;
    const std::filesystem::path file = pathOf(folder.filePath(QStringLiteral("assets.json")));
    writeFile(file, kConfig);
    AssetServersDialog dialog(nullptr, file);
    auto* logins = dialog.findChild<QTableWidget*>(QStringLiteral("assetServersLogins"));
    auto* servers = dialog.findChild<QTableWidget*>(QStringLiteral("assetServersTable"));
    auto* forget = dialog.findChild<QPushButton*>(QStringLiteral("assetServersForgetLogin"));
    CHECK(logins && logins->rowCount() == 2, "the saved login and the one a server names");
    const int test = rowOf(logins, QStringLiteral("test")), studio = rowOf(logins, QStringLiteral("studio"));
    CHECK(test >= 0 && studio >= 0, "by their ids");
    if (test < 0 || studio < 0) return;
    CHECK(logins->item(test, 3)->text() == QStringLiteral("Saved") && logins->item(test, 2)->text() == QStringLiteral("ada"),
          "saved, with its user");
    CHECK(logins->item(test, 4)->text() == QStringLiteral("Nothing configured"), "used by nothing");
    CHECK(logins->item(studio, 3)->text() == QStringLiteral("No login") &&
              logins->item(studio, 4)->text().contains(QStringLiteral("'prod'")),
          "and one without a login, with its server");
    dialog.showLogins(QStringLiteral("studio"));
    CHECK(!forget->isEnabled(), "there is nothing to forget of that one");
    dialog.showLogins(QStringLiteral("test"));
    CHECK(forget->isEnabled(), "but of the saved one");

    // Giving a server the credential: both tables follow.
    const int server = rowOf(servers, QStringLiteral("test"));
    servers->item(server, 3)->setText(QStringLiteral("test"));
    CHECK(servers->item(server, 4)->text() == QStringLiteral("Saved"), "the server's login is saved");
    CHECK(logins->item(rowOf(logins, QStringLiteral("test")), 4)->text().contains(QStringLiteral("'test'")),
          "and the login says the server uses it");
    servers->item(server, 2)->setText(QStringLiteral("https://locked.example"));
    dialog.testServer(QStringLiteral("test"));
    CHECK(waitFor([&]() { return servers->item(rowOf(servers, QStringLiteral("test")), 4)->text() == QStringLiteral("Refused"); }),
          "a Test the server refuses shows");

    QTimer::singleShot(0, []() {
        if (auto* ask = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            CHECK(ask->text().contains(QStringLiteral("'test'")), "Forget names what uses it");
            ask->button(QMessageBox::Yes)->click();
        }
    });
    dialog.showLogins(QStringLiteral("test"));
    forget->click();
    CHECK(!store->logins.count("test"), "forgotten");
    CHECK(logins->item(rowOf(logins, QStringLiteral("test")), 3)->text() == QStringLiteral("No login"),
          "and still listed, as a server names it");
}
