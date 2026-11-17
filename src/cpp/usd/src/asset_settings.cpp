#include "usdcc/usd/asset_settings.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/credentials.h"
#include "usdcc/assets/disk_cache.h"
#include "usdcc/assets/lookup.h"
#include "usdcc/assets/paths.h"
#include "usdcc/core/log.h"
#include "usdcc/usd/asset_servers.h"

#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace usdcc::usd {

namespace {

constexpr const char* kCategory = "Assets";

std::vector<std::string> splitHosts(const std::string& text) {
    std::vector<std::string> hosts;
    std::string current;
    for (const char c : text + ";") {
        if (c == ';' || c == ',' || c == ' ') {
            if (!current.empty()) hosts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    return hosts;
}

void apply(core::Settings& settings, const std::string& key) {
    if (key.empty() || key == kAssetEnvironmentKey) {
        setPreferredAssetEnvironment(settings.get<std::string>(kAssetEnvironmentKey));
    }
    if (key.empty() || key == kAssetOfflineKey) usdcc::assets::setOffline(settings.get<bool>(kAssetOfflineKey));
    if (key.empty() || key == kAssetCacheLimitKey) {
        const double gigabytes = settings.get<double>(kAssetCacheLimitKey);
        usdcc::assets::DiskCache::shared().setLimit(static_cast<uint64_t>(gigabytes * 1024.0 * 1024.0 * 1024.0));
    }
    if (key.empty() || key == kAssetHttpHostsKey) {
        usdcc::assets::setExtraAllowedHosts(splitHosts(settings.get<std::string>(kAssetHttpHostsKey)));
        // A host refused this session may be on the list now.
        usdcc::assets::forgetHostAnswers();
    }
}

std::string environmentsText() {
    const std::vector<std::string> environments = assetEnvironments();
    if (environments.empty()) return "The configuration names no environments.";
    std::string text = "The configuration's: ";
    for (size_t i = 0; i < environments.size(); ++i) text += (i ? ", " : "") + environments[i];
    return text + ".";
}

core::Signal<const std::string&>::Connection& settingsConnection() {
    static core::Signal<const std::string&>::Connection connection;
    return connection;
}

// What the window shows of downloads: the one running now, and how many are.
class StatusActivity : public usdcc::assets::ActivityListener {
public:
    explicit StatusActivity(QMainWindow* window) : m_window(window) {}

    void started(const std::string& url) override {
        ++m_running;
        show(url, 0, 0);
    }
    bool advanced(const std::string& url, uint64_t done, uint64_t total) override {
        // Not every call: they come many times a second.
        if (done >= m_lastShown + 1024 * 1024 || (total && done == total)) {
            m_lastShown = done;
            show(url, done, total);
        }
        return true;
    }
    void finished(const std::string& url, usdcc::assets::Status status) override {
        m_lastShown = 0;
        const int left = --m_running;
        QPointer<QMainWindow> window = m_window;
        const QString name = QFileInfo(QString::fromStdString(url)).fileName();
        const bool failed = status != usdcc::assets::Status::Ok;
        QMetaObject::invokeMethod(
            m_window,
            [window, name, left, failed]() {
                if (!window) return;
                if (failed) {
                    window->statusBar()->showMessage(QObject::tr("Couldn't download %1").arg(name), 5000);
                } else if (left <= 0) {
                    window->statusBar()->showMessage(QObject::tr("Downloaded %1").arg(name), 2000);
                }
            },
            Qt::QueuedConnection);
    }

private:
    void show(const std::string& url, uint64_t done, uint64_t total) {
        QPointer<QMainWindow> window = m_window;
        const QString name = QFileInfo(QString::fromStdString(url)).fileName();
        const int running = m_running.load();
        QMetaObject::invokeMethod(
            m_window,
            [window, name, done, total, running]() {
                if (!window) return;
                QString text = QObject::tr("Downloading %1").arg(name);
                if (total) {
                    text += QObject::tr(" (%1 of %2 MB)").arg(double(done) / 1048576.0, 0, 'f', 1).arg(double(total) / 1048576.0, 0, 'f', 1);
                } else if (done) {
                    text += QObject::tr(" (%1 MB)").arg(double(done) / 1048576.0, 0, 'f', 1);
                }
                if (running > 1) text += QObject::tr(", %1 more").arg(running - 1);
                window->statusBar()->showMessage(text, 10000);
            },
            Qt::QueuedConnection);
    }

    QPointer<QMainWindow> m_window;
    std::atomic<int> m_running{0};
    std::atomic<uint64_t> m_lastShown{0};
};

}  // namespace

void defineAssetSettings(core::Settings& settings) {
    using core::SettingType;
    const std::string config = usdcc::assets::toUtf8(usdcc::assets::Config::defaultPath());
    settings.define({.key = kAssetEnvironmentKey,
                     .category = kCategory,
                     .group = "Servers",
                     .label = "Environment",
                     .description = "Which servers a path such as webdav://studio/... means, and in which order: TEST "
                                    "over PROD, say. Empty for the configuration's own choice. Open Stage with Options "
                                    "can open one stage in another. " +
                                    environmentsText() + " The servers are in " + config + ".",
                     .type = SettingType::String,
                     .defaultValue = std::string()});
    settings.define({.key = kAssetOfflineKey,
                     .category = kCategory,
                     .group = "Servers",
                     .label = "Work offline",
                     .description = "Asks no server: files on a server open from the copies already downloaded, and "
                                    "the rest are missing.",
                     .type = SettingType::Bool,
                     .defaultValue = false});
    settings.define({.key = kAssetHttpHostsKey,
                     .category = kCategory,
                     .group = "Web",
                     .label = "Allowed hosts",
                     .description = "The hosts a stage may load http:// and https:// files from, separated by ';': a "
                                    "name (assets.example.com), every host under one (*.studio.local), or * for any. "
                                    "A stage can name any address, so others are refused.",
                     .type = SettingType::String,
                     .defaultValue = std::string()});
    settings.define({.key = kAssetCacheLimitKey,
                     .category = kCategory,
                     .group = "Cache",
                     .label = "Cache size (GB)",
                     .description = "How much the downloaded copies may take on disk. Those used longest ago go first. "
                                    "They are in " +
                                    usdcc::assets::toUtf8(usdcc::assets::DiskCache::shared().root()) + ".",
                     .type = SettingType::Float,
                     .defaultValue = 20.0,
                     .minimum = 0.5,
                     .maximum = 10000.0});
    apply(settings, {});
    settingsConnection() = settings.changed.connect([&settings](const std::string& key) {
        if (key.rfind("Assets/", 0) == 0) apply(settings, key);
    });
}

void installAssetActivity(QMainWindow* window) {
    usdcc::assets::setActivityListener(std::make_shared<StatusActivity>(window));
    // Asked from whichever thread resolves, which may be one the UI thread waits for:
    // so no question, but a note, once per host, and no.
    usdcc::assets::setHostCallback([](const std::string& host) {
        static std::mutex mutex;
        static std::set<std::string> told;
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            first = told.insert(host).second;
        }
        if (first) {
            USDCC_LOG_TOAST_STICKY(core::LogLevel::Warning, "usdcc.assets",
                                   "A stage asked for files from {}, which isn't an allowed host. To allow it, add it "
                                   "in Settings > Assets > Allowed hosts, then reload the stage.",
                                   host);
        }
        return usdcc::assets::HostAnswer::No;
    });
}

QString displayPath(const QString& path) {
    const std::string text = path.toStdString();
    if (isLocalAssetPath(text)) return QDir::toNativeSeparators(path);
    return QString::fromStdString(displayAssetPath(text));
}

QString askForAssetUrl(QWidget* parent) {
    bool ok = false;
    const QString url =
        QInputDialog::getText(parent, QObject::tr("Open Stage from URL"),
                              QObject::tr("A stage on a server, such as webdav://studio/shots/s010.usda or "
                                          "https://example.com/scene.usdz:"),
                              QLineEdit::Normal, QString(), &ok)
            .trimmed();
    return ok ? url : QString();
}

void showAssetLoginDialog(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Asset Server Login"));
    dialog.setObjectName(QStringLiteral("assetLoginDialog"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(QObject::tr("Saved in Windows Credential Manager, under the credential id the asset "
                                         "configuration names for a server. For a server opened by its address "
                                         "(webdav://192.168.1.5:5000/...), the address: 192.168.1.5, or "
                                         "192.168.1.5:5000 for that port only."),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    auto* id = new QComboBox(&dialog);
    id->setObjectName(QStringLiteral("credentialId"));
    id->setEditable(true);
    std::set<std::string> ids;
    const usdcc::assets::Config config = usdcc::assets::defaultConfig();
    for (const auto& [name, endpoint] : config.endpoints) {
        if (!endpoint.credential.empty()) ids.insert(endpoint.credential);
    }
    for (const auto& [host, entry] : config.httpHosts) {
        if (!entry.credential.empty()) ids.insert(entry.credential);
    }
    for (const std::string& each : ids) id->addItem(QString::fromStdString(each));
    form->addRow(QObject::tr("Credential:"), id);

    auto* userMode = new QRadioButton(QObject::tr("User and password"), &dialog);
    auto* tokenMode = new QRadioButton(QObject::tr("Token"), &dialog);
    userMode->setChecked(true);
    auto* user = new QLineEdit(&dialog);
    user->setObjectName(QStringLiteral("credentialUser"));
    auto* secret = new QLineEdit(&dialog);
    secret->setObjectName(QStringLiteral("credentialSecret"));
    secret->setEchoMode(QLineEdit::Password);
    form->addRow(userMode);
    form->addRow(QObject::tr("User:"), user);
    form->addRow(tokenMode);
    form->addRow(QObject::tr("Password or token:"), secret);
    layout->addLayout(form);
    QObject::connect(tokenMode, &QRadioButton::toggled, user, [user](bool token) { user->setEnabled(!token); });
    const auto showStored = [id, user, userMode, tokenMode]() {
        const std::optional<usdcc::assets::Credential> stored = usdcc::assets::storedCredential(id->currentText().toStdString());
        user->setText(stored ? QString::fromStdString(stored->user) : QString());
        (stored && !stored->token.empty() ? tokenMode : userMode)->setChecked(true);
    };
    QObject::connect(id, &QComboBox::currentTextChanged, &dialog, showStored);
    showStored();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QPushButton* forget = buttons->addButton(QObject::tr("Forget"), QDialogButtonBox::DestructiveRole);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(forget, &QPushButton::clicked, &dialog, [&dialog, id]() {
        usdcc::assets::forgetStoredCredential(id->currentText().toStdString());
        dialog.reject();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, id, user, secret, tokenMode]() {
        const std::string credentialId = id->currentText().trimmed().toStdString();
        if (credentialId.empty()) return;
        usdcc::assets::Credential credential;
        if (tokenMode->isChecked()) {
            credential.token = secret->text().toStdString();
        } else {
            credential.user = user->text().toStdString();
            credential.password = secret->text().toStdString();
        }
        std::string error;
        if (!usdcc::assets::storeCredential(credentialId, credential, &error)) {
            QMessageBox::warning(&dialog, QObject::tr("Asset Server Login"),
                                 QObject::tr("It couldn't be saved: %1").arg(QString::fromStdString(error)));
            return;
        }
        // What servers answered without it is forgotten, so the next resolve asks again.
        usdcc::assets::forgetLookups();
        dialog.accept();
    });
    dialog.setMinimumWidth(420);
    dialog.exec();
}

}  // namespace usdcc::usd
