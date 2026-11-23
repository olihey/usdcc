#include "usdcc/usd/asset_servers_editor.h"
#include "usdcc/ui/theme.h"

#include "usdcc/assets/config_editor.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/paths.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/asset_settings.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/base/tf/getenv.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTabWidget>
#include <QTableWidget>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace usdcc::usd {

namespace {

namespace assets = usdcc::assets;

enum ServerColumn { kId, kProtocol, kUrl, kCredential, kReadOnly, kOptional, kServerColumns };
enum WebHostColumn { kHost, kHostCredential, kInsecure, kWebHostColumns };

// Sets a flag for its scope and puts back what it was: a refresh inside a refresh (a
// protocol box changing while its row is made) doesn't clear the outer one's.
struct RefreshGuard {
    explicit RefreshGuard(bool& flag) : flag(flag), was(flag) { flag = true; }
    ~RefreshGuard() { flag = was; }
    bool& flag;
    bool was;
};

const StageManager*& stageManagerPointer() {
    static const StageManager* stages = nullptr;
    return stages;
}

QString qs(const std::string& text) { return QString::fromStdString(text); }

std::vector<std::string> splitList(const QString& text) {
    std::vector<std::string> items;
    for (const QString& part : text.split(QRegularExpression(QStringLiteral("[;,\\s]+")), Qt::SkipEmptyParts)) {
        items.push_back(part.toStdString());
    }
    return items;
}

QString joinList(const std::vector<std::string>& items) {
    QStringList parts;
    for (const std::string& item : items) parts << qs(item);
    return parts.join(QStringLiteral("; "));
}

// The protocols a server can be: those with a named transport, never plain HTTP, which has
// hosts rather than servers (§5.1).
QStringList serverProtocols(const QString& current) {
    assets::loadProtocolPlugins();
    std::set<std::string> schemes{"webdav", "s3"};
    for (const std::string& scheme : assets::transportSchemes()) schemes.insert(scheme);
    schemes.erase("http");
    schemes.erase("https");
    schemes.erase("file");
    if (!current.isEmpty()) schemes.insert(current.toStdString());
    QStringList result;
    for (const std::string& scheme : schemes) result << qs(scheme);
    return result;
}

QTableWidgetItem* textItem(const QString& text) { return new QTableWidgetItem(text); }

QTableWidgetItem* flagItem(bool on) {
    auto* item = new QTableWidgetItem();
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    return item;
}

// Each environment as its stacks, servers included: what a save changed for its stages.
std::map<std::string, std::string> environmentFingerprints(const assets::Config& config) {
    std::map<std::string, std::string> result;
    for (const auto& [environment, schemes] : config.environments) {
        std::string text;
        for (const auto& [scheme, names] : schemes) {
            for (const auto& [name, servers] : names) {
                text += scheme + "://" + name + "=";
                for (const std::string& server : servers) {
                    const auto found = config.endpoints.find(server);
                    if (found == config.endpoints.end()) continue;
                    const assets::Endpoint& endpoint = found->second;
                    text += endpoint.id + "|" + endpoint.url + "|" + endpoint.credential + "|" +
                            (endpoint.readOnly ? "r" : "") + (endpoint.optional ? "o" : "") + ";";
                    for (const std::string& mirror : endpoint.mirrors) text += mirror + ",";
                    for (const auto& [key, value] : endpoint.options) text += key + "=" + value + ",";
                }
                text += "\n";
            }
        }
        result[environment] = text;
    }
    return result;
}

}  // namespace

void AssetServersDialog::setStageManager(const StageManager* stages) { stageManagerPointer() = stages; }

AssetServersDialog::AssetServersDialog(QWidget* parent, const std::filesystem::path& file) : QDialog(parent) {
    setWindowTitle(tr("Asset Servers"));
    setObjectName(QStringLiteral("assetServersDialog"));
    m_editor = std::make_unique<assets::ConfigEditor>(file.empty() ? assets::Config::defaultPath() : file);
    build();
    refreshAll();
    resize(820, 560);
}

AssetServersDialog::~AssetServersDialog() = default;

void AssetServersDialog::build() {
    auto* layout = new QVBoxLayout(this);
    m_fileLabel = new QLabel(this);
    m_fileLabel->setObjectName(QStringLiteral("assetServersFile"));
    m_fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_fileLabel);
    m_notice = new QLabel(this);
    m_notice->setObjectName(QStringLiteral("assetServersNotice"));
    m_notice->setWordWrap(true);
    layout->addWidget(m_notice);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    // ---- Servers -------------------------------------------------------------------------
    auto* serversPage = new QWidget(m_tabs);
    auto* serversLayout = new QVBoxLayout(serversPage);
    m_servers = new QTableWidget(0, kServerColumns, serversPage);
    m_servers->setObjectName(QStringLiteral("assetServersTable"));
    m_servers->setHorizontalHeaderLabels(
        {tr("Id"), tr("Protocol"), tr("URL"), tr("Credential"), tr("Read-only"), tr("Optional")});
    m_servers->horizontalHeader()->setSectionResizeMode(kUrl, QHeaderView::Stretch);
    m_servers->verticalHeader()->hide();
    m_servers->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_servers->setSelectionMode(QAbstractItemView::SingleSelection);
    serversLayout->addWidget(m_servers, 1);
    connect(m_servers, &QTableWidget::cellChanged, this, &AssetServersDialog::serverCellChanged);
    connect(m_servers, &QTableWidget::itemSelectionChanged, this, &AssetServersDialog::refreshServerDetails);

    auto* serverButtons = new QHBoxLayout();
    m_addServer = new QPushButton(tr("Add Server..."), serversPage);
    m_removeServer = new QPushButton(tr("Remove"), serversPage);
    serverButtons->addWidget(m_addServer);
    serverButtons->addWidget(m_removeServer);
    serverButtons->addStretch(1);
    m_test = new QPushButton(tr("Test"), serversPage);
    m_test->setObjectName(QStringLiteral("assetServersTest"));
    m_test->setToolTip(tr("Asks the server for its root folder, with the table's values as they are now"));
    m_login = new QPushButton(tr("Login..."), serversPage);
    serverButtons->addWidget(m_test);
    serverButtons->addWidget(m_login);
    serversLayout->addLayout(serverButtons);
    m_testResult = new QLabel(serversPage);
    m_testResult->setObjectName(QStringLiteral("assetServersTestResult"));
    m_testResult->setWordWrap(true);
    m_testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    serversLayout->addWidget(m_testResult);

    auto* details = new QHBoxLayout();
    auto* mirrorsBox = new QVBoxLayout();
    mirrorsBox->addWidget(new QLabel(tr("Mirrors (other URLs with the same files, tried when it doesn't answer):"),
                                     serversPage));
    m_mirrors = new QListWidget(serversPage);
    m_mirrors->setObjectName(QStringLiteral("assetServersMirrors"));
    m_mirrors->setMaximumHeight(90);
    mirrorsBox->addWidget(m_mirrors);
    auto* mirrorButtons = new QHBoxLayout();
    m_addMirror = new QPushButton(tr("Add Mirror..."), serversPage);
    m_removeMirror = new QPushButton(tr("Remove Mirror"), serversPage);
    mirrorButtons->addWidget(m_addMirror);
    mirrorButtons->addWidget(m_removeMirror);
    mirrorButtons->addStretch(1);
    mirrorsBox->addLayout(mirrorButtons);
    details->addLayout(mirrorsBox, 2);

    // S3's own fields (docs/S3_RESOLVER.md §7): the bucket and its prefix are the URL
    // (s3://bucket/prefix, or http(s)://host/bucket/prefix); these say where it is.
    m_s3Fields = new QWidget(serversPage);
    auto* s3Form = new QFormLayout(m_s3Fields);
    s3Form->setContentsMargins(0, 0, 0, 0);
    m_s3Region = new QLineEdit(m_s3Fields);
    m_s3Region->setObjectName(QStringLiteral("assetServersS3Region"));
    m_s3Region->setPlaceholderText(tr("found by asking, when empty"));
    m_s3BucketInHost = new QCheckBox(tr("Bucket in the host name"), m_s3Fields);
    m_s3BucketInHost->setObjectName(QStringLiteral("assetServersS3BucketInHost"));
    m_s3BucketInHost->setToolTip(tr("http(s)://<bucket>.<host>/<prefix> rather than http(s)://<host>/<bucket>/<prefix>"));
    auto* s3Hint = new QLabel(tr("The URL is s3://<bucket>/<prefix> for AWS, or http(s)://<host>/<bucket>/<prefix>."),
                              m_s3Fields);
    s3Hint->setWordWrap(true);
    s3Form->addRow(s3Hint);
    s3Form->addRow(tr("Region:"), m_s3Region);
    s3Form->addRow(m_s3BucketInHost);
    details->addWidget(m_s3Fields, 1);
    serversLayout->addLayout(details);
    m_tabs->addTab(serversPage, tr("Servers"));

    connect(m_addServer, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString id = QInputDialog::getText(this, tr("Add Server"), tr("Id (as stacks name it, such as prod):"),
                                                 QLineEdit::Normal, QString(), &ok)
                               .trimmed();
        if (!ok || id.isEmpty()) return;
        assets::Endpoint endpoint;
        endpoint.id = id.toStdString();
        endpoint.scheme = "webdav";
        endpoint.url = "https://";
        const std::string why = m_editor->addServer(endpoint);
        if (!why.empty()) return report(qs(why));
        refreshServers();
        selectServer(id);
        refreshProblems();
    });
    connect(m_removeServer, &QPushButton::clicked, this, [this]() {
        const QString id = selectedServer();
        if (id.isEmpty()) return;
        const std::string why = m_editor->removeServer(id.toStdString());
        if (!why.empty()) return report(qs(why));
        refreshAll();
    });
    connect(m_addMirror, &QPushButton::clicked, this, [this]() {
        const QString id = selectedServer();
        auto found = m_editor->config().endpoints.find(id.toStdString());
        if (found == m_editor->config().endpoints.end()) return;
        bool ok = false;
        const QString url =
            QInputDialog::getText(this, tr("Add Mirror"), tr("URL:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || url.isEmpty()) return;
        found->second.mirrors.push_back(url.toStdString());
        refreshServerDetails();
        refreshProblems();
    });
    connect(m_removeMirror, &QPushButton::clicked, this, [this]() {
        auto found = m_editor->config().endpoints.find(selectedServer().toStdString());
        const int row = m_mirrors->currentRow();
        if (found == m_editor->config().endpoints.end() || row < 0 ||
            row >= int(found->second.mirrors.size())) {
            return;
        }
        found->second.mirrors.erase(found->second.mirrors.begin() + row);
        refreshServerDetails();
        refreshProblems();
    });
    connect(m_s3Region, &QLineEdit::textEdited, this, [this](const QString& text) {
        auto found = m_editor->config().endpoints.find(selectedServer().toStdString());
        if (found == m_editor->config().endpoints.end()) return;
        if (text.trimmed().isEmpty()) {
            found->second.options.erase("region");
        } else {
            found->second.options["region"] = text.trimmed().toStdString();
        }
        refreshProblems();
    });
    connect(m_s3BucketInHost, &QCheckBox::toggled, this, [this](bool on) {
        if (m_refreshing) return;
        auto found = m_editor->config().endpoints.find(selectedServer().toStdString());
        if (found == m_editor->config().endpoints.end()) return;
        if (on) {
            found->second.options["bucketInHost"] = "true";
        } else {
            found->second.options.erase("bucketInHost");
        }
        refreshProblems();
    });
    connect(m_test, &QPushButton::clicked, this, [this]() { testServer(); });
    connect(m_login, &QPushButton::clicked, this, [this]() {
        const auto found = m_editor->config().endpoints.find(selectedServer().toStdString());
        if (found == m_editor->config().endpoints.end()) return;
        showAssetLoginDialog(this, qs(found->second.credential), qs(found->second.scheme));
    });

    // ---- Environments --------------------------------------------------------------------
    auto* environmentsPage = new QWidget(m_tabs);
    auto* environmentsLayout = new QHBoxLayout(environmentsPage);
    auto* left = new QVBoxLayout();
    left->addWidget(new QLabel(tr("Environments:"), environmentsPage));
    m_environments = new QListWidget(environmentsPage);
    m_environments->setObjectName(QStringLiteral("assetServersEnvironments"));
    left->addWidget(m_environments, 1);
    auto* environmentButtons = new QHBoxLayout();
    m_addEnvironment = new QPushButton(tr("Add..."), environmentsPage);
    m_duplicateEnvironment = new QPushButton(tr("Duplicate..."), environmentsPage);
    m_duplicateEnvironment->setToolTip(tr("A copy of this one: how a test environment usually starts, with a test "
                                          "server then dragged on top"));
    m_renameEnvironment = new QPushButton(tr("Rename..."), environmentsPage);
    m_removeEnvironment = new QPushButton(tr("Remove"), environmentsPage);
    environmentButtons->addWidget(m_addEnvironment);
    environmentButtons->addWidget(m_duplicateEnvironment);
    environmentButtons->addWidget(m_renameEnvironment);
    environmentButtons->addWidget(m_removeEnvironment);
    left->addLayout(environmentButtons);
    environmentsLayout->addLayout(left, 1);

    auto* middle = new QVBoxLayout();
    middle->addWidget(new QLabel(tr("Names in it:"), environmentsPage));
    m_names = new QTreeWidget(environmentsPage);
    m_names->setObjectName(QStringLiteral("assetServersNames"));
    m_names->setHeaderHidden(true);
    m_names->setRootIsDecorated(false);
    middle->addWidget(m_names, 1);
    auto* nameButtons = new QHBoxLayout();
    m_addName = new QPushButton(tr("Add Name..."), environmentsPage);
    m_removeName = new QPushButton(tr("Remove Name"), environmentsPage);
    nameButtons->addWidget(m_addName);
    nameButtons->addWidget(m_removeName);
    nameButtons->addStretch(1);
    middle->addLayout(nameButtons);
    environmentsLayout->addLayout(middle, 1);

    auto* right = new QVBoxLayout();
    right->addWidget(new QLabel(tr("Its stack, top first (drag to reorder):"), environmentsPage));
    m_stack = new QListWidget(environmentsPage);
    m_stack->setObjectName(QStringLiteral("assetServersStack"));
    m_stack->setDragDropMode(QAbstractItemView::InternalMove);
    m_stack->setDefaultDropAction(Qt::MoveAction);
    right->addWidget(m_stack, 1);
    auto* stackButtons = new QHBoxLayout();
    m_stackServer = new QComboBox(environmentsPage);
    m_stackServer->setObjectName(QStringLiteral("assetServersStackServer"));
    m_addToStack = new QPushButton(tr("Add"), environmentsPage);
    m_removeFromStack = new QPushButton(tr("Remove"), environmentsPage);
    stackButtons->addWidget(m_stackServer, 1);
    stackButtons->addWidget(m_addToStack);
    stackButtons->addWidget(m_removeFromStack);
    right->addLayout(stackButtons);
    environmentsLayout->addLayout(right, 1);
    m_tabs->addTab(environmentsPage, tr("Environments"));

    connect(m_environments, &QListWidget::currentRowChanged, this, [this]() { refreshNames(); });
    connect(m_names, &QTreeWidget::currentItemChanged, this, [this]() { refreshStack(); });
    // A drop reorders the list's rows; the model follows once the drop is done.
    connect(m_stack->model(), &QAbstractItemModel::rowsMoved, this, [this]() { applyStackOrder(); });
    const auto askName = [this](const QString& title, const QString& text, const QString& value = {}) {
        bool ok = false;
        const QString name =
            QInputDialog::getText(this, title, text, QLineEdit::Normal, value, &ok).trimmed();
        return ok ? name : QString();
    };
    connect(m_addEnvironment, &QPushButton::clicked, this, [this, askName]() {
        const QString name = askName(tr("Add Environment"), tr("Name (such as test):"));
        if (name.isEmpty()) return;
        const std::string why = m_editor->addEnvironment(name.toStdString());
        if (!why.empty()) return report(qs(why));
        refreshEnvironments();
        selectEnvironment(name);
        refreshDefaultEnvironment();
        refreshProblems();
    });
    connect(m_duplicateEnvironment, &QPushButton::clicked, this, [this, askName]() {
        const QString from = selectedEnvironment();
        if (from.isEmpty()) return;
        const QString name = askName(tr("Duplicate Environment"), tr("Name of the copy of %1:").arg(from));
        if (name.isEmpty()) return;
        const std::string why = m_editor->duplicateEnvironment(from.toStdString(), name.toStdString());
        if (!why.empty()) return report(qs(why));
        refreshEnvironments();
        selectEnvironment(name);
        refreshDefaultEnvironment();
        refreshProblems();
    });
    connect(m_renameEnvironment, &QPushButton::clicked, this, [this, askName]() {
        const QString from = selectedEnvironment();
        if (from.isEmpty()) return;
        const QString name = askName(tr("Rename Environment"), tr("New name:"), from);
        if (name.isEmpty()) return;
        const std::string why = m_editor->renameEnvironment(from.toStdString(), name.toStdString());
        if (!why.empty()) return report(qs(why));
        refreshEnvironments();
        selectEnvironment(name);
        refreshDefaultEnvironment();
        refreshProblems();
    });
    connect(m_removeEnvironment, &QPushButton::clicked, this, [this]() {
        const QString environment = selectedEnvironment();
        if (environment.isEmpty()) return;
        const std::string why = m_editor->removeEnvironment(environment.toStdString());
        if (!why.empty()) return report(qs(why));
        refreshEnvironments();
        refreshDefaultEnvironment();
        refreshProblems();
    });
    connect(m_addName, &QPushButton::clicked, this, [this]() {
        const QString environment = selectedEnvironment();
        if (environment.isEmpty()) return;
        QStringList servers;
        for (const auto& [id, endpoint] : m_editor->config().endpoints) {
            servers << qs(id) + QStringLiteral("  (") + qs(endpoint.scheme) + QStringLiteral(")");
        }
        if (servers.isEmpty()) return report(tr("Add a server first: a name's stack is made of servers."));
        QDialog ask(this);
        ask.setWindowTitle(tr("Add Name"));
        auto* form = new QFormLayout(&ask);
        auto* name = new QLineEdit(&ask);
        name->setObjectName(QStringLiteral("assetServersNewName"));
        name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[a-z0-9._-]*")), name));
        name->setPlaceholderText(tr("lowercase letters, digits, '-', '_' and '.'"));
        auto* first = new QComboBox(&ask);
        first->addItems(servers);
        form->addRow(tr("Name (webdav://<name>/...):"), name);
        form->addRow(tr("First server:"), first);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &ask);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &ask, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &ask, &QDialog::reject);
        if (ask.exec() != QDialog::Accepted) return;
        const QString server = first->currentText().section(QStringLiteral("  ("), 0, 0);
        const std::string why =
            m_editor->addName(environment.toStdString(), name->text().toStdString(), server.toStdString());
        if (!why.empty()) return report(qs(why));
        refreshNames();
        selectName(qs(m_editor->config().endpoints.at(server.toStdString()).scheme), name->text());
        refreshProblems();
    });
    connect(m_removeName, &QPushButton::clicked, this, [this]() {
        std::string scheme, name;
        if (!selectedName(&scheme, &name)) return;
        const std::string why = m_editor->removeName(selectedEnvironment().toStdString(), scheme, name);
        if (!why.empty()) return report(qs(why));
        refreshNames();
        refreshProblems();
    });
    connect(m_addToStack, &QPushButton::clicked, this, [this]() {
        std::string scheme, name;
        if (!selectedName(&scheme, &name) || m_stackServer->currentText().isEmpty()) return;
        const std::string environment = selectedEnvironment().toStdString();
        std::vector<std::string> servers = m_editor->config().environments[environment][scheme][name];
        servers.push_back(m_stackServer->currentText().toStdString());
        const std::string why = m_editor->setStack(environment, scheme, name, servers);
        if (!why.empty()) return report(qs(why));
        refreshStack();
        refreshProblems();
    });
    connect(m_removeFromStack, &QPushButton::clicked, this, [this]() {
        std::string scheme, name;
        const int row = m_stack->currentRow();
        if (!selectedName(&scheme, &name) || row < 0) return;
        const std::string environment = selectedEnvironment().toStdString();
        std::vector<std::string> servers = m_editor->config().environments[environment][scheme][name];
        if (row >= int(servers.size())) return;
        servers.erase(servers.begin() + row);
        const std::string why = m_editor->setStack(environment, scheme, name, servers);
        if (!why.empty()) return report(qs(why));
        refreshStack();
        refreshProblems();
    });

    // ---- Web hosts -----------------------------------------------------------------------
    auto* webPage = new QWidget(m_tabs);
    auto* webLayout = new QVBoxLayout(webPage);
    auto* webIntro = new QLabel(tr("Hosts of plain http:// and https:// with a login, and which hosts and S3 buckets a "
                                   "stage may name by themselves. This is the configuration's list, which tools "
                                   "outside usdcc read too; your own additions stay in Settings > Assets."),
                                webPage);
    webIntro->setWordWrap(true);
    webLayout->addWidget(webIntro);
    m_webHosts = new QTableWidget(0, kWebHostColumns, webPage);
    m_webHosts->setObjectName(QStringLiteral("assetServersWebHosts"));
    m_webHosts->setHorizontalHeaderLabels({tr("Host"), tr("Credential"), tr("Insecure (login over http://)")});
    m_webHosts->horizontalHeader()->setSectionResizeMode(kHost, QHeaderView::Stretch);
    m_webHosts->verticalHeader()->hide();
    m_webHosts->setSelectionBehavior(QAbstractItemView::SelectRows);
    webLayout->addWidget(m_webHosts, 1);
    connect(m_webHosts, &QTableWidget::cellChanged, this, &AssetServersDialog::webHostCellChanged);
    auto* webButtons = new QHBoxLayout();
    m_addWebHost = new QPushButton(tr("Add Host..."), webPage);
    m_removeWebHost = new QPushButton(tr("Remove"), webPage);
    webButtons->addWidget(m_addWebHost);
    webButtons->addWidget(m_removeWebHost);
    webButtons->addStretch(1);
    webLayout->addLayout(webButtons);
    auto* allowForm = new QFormLayout();
    m_httpAllow = new QLineEdit(webPage);
    m_httpAllow->setObjectName(QStringLiteral("assetServersHttpAllow"));
    m_httpAllow->setPlaceholderText(tr("*.studio.local; assets.example.com"));
    m_s3Allow = new QLineEdit(webPage);
    m_s3Allow->setObjectName(QStringLiteral("assetServersS3Allow"));
    m_s3Allow->setPlaceholderText(tr("studio-*"));
    allowForm->addRow(tr("Allowed hosts:"), m_httpAllow);
    allowForm->addRow(tr("Allowed S3 buckets:"), m_s3Allow);
    webLayout->addLayout(allowForm);
    m_tabs->addTab(webPage, tr("Web Hosts"));

    connect(m_addWebHost, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString host = QInputDialog::getText(this, tr("Add Host"), tr("Host (such as assets.example.com):"),
                                                   QLineEdit::Normal, QString(), &ok)
                                 .trimmed();
        if (!ok || host.isEmpty()) return;
        m_editor->config().httpHosts[host.toStdString()];
        refreshWebHosts();
        refreshProblems();
    });
    connect(m_removeWebHost, &QPushButton::clicked, this, [this]() {
        const int row = m_webHosts->currentRow();
        if (row < 0) return;
        m_editor->config().httpHosts.erase(m_webHosts->item(row, kHost)->data(Qt::UserRole).toString().toStdString());
        refreshWebHosts();
        refreshProblems();
    });
    connect(m_httpAllow, &QLineEdit::textEdited, this, [this](const QString& text) {
        m_editor->config().httpAllow = splitList(text);
        refreshProblems();
    });
    connect(m_s3Allow, &QLineEdit::textEdited, this, [this](const QString& text) {
        m_editor->config().s3Allow = splitList(text);
        refreshProblems();
    });

    // ---- Bottom --------------------------------------------------------------------------
    m_problems = new QLabel(this);
    m_problems->setObjectName(QStringLiteral("assetServersProblems"));
    m_problems->setWordWrap(true);
    m_problems->setStyleSheet(QStringLiteral("color: %1;").arg(usdcc::ui::theme::color(usdcc::ui::theme::Token::Error).name()));
    layout->addWidget(m_problems);

    auto* bottom = new QHBoxLayout();
    bottom->addWidget(new QLabel(tr("Default environment:"), this));
    m_defaultEnvironment = new QComboBox(this);
    m_defaultEnvironment->setObjectName(QStringLiteral("assetServersDefaultEnvironment"));
    m_defaultEnvironment->setToolTip(tr("The configuration's choice. Settings > Assets > Environment can still choose "
                                        "another for you."));
    bottom->addWidget(m_defaultEnvironment);
    bottom->addStretch(1);
    m_saveCopy = new QPushButton(tr("Save a Copy as Mine"), this);
    m_saveCopy->setObjectName(QStringLiteral("assetServersSaveCopy"));
    m_reload = new QPushButton(tr("Reload"), this);
    m_save = new QPushButton(tr("Save"), this);
    m_save->setObjectName(QStringLiteral("assetServersSave"));
    m_save->setDefault(true);
    auto* close = new QPushButton(tr("Close"), this);
    bottom->addWidget(m_saveCopy);
    bottom->addWidget(m_reload);
    bottom->addWidget(m_save);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    connect(m_defaultEnvironment, &QComboBox::currentIndexChanged, this, [this]() {
        if (m_refreshing) return;
        m_editor->config().environment = m_defaultEnvironment->currentData().toString().toStdString();
        refreshProblems();
    });
    connect(m_save, &QPushButton::clicked, this, [this]() { save(); });
    connect(m_saveCopy, &QPushButton::clicked, this, [this]() { saveCopyAsMine(); });
    connect(m_reload, &QPushButton::clicked, this, [this]() {
        if (m_editor->modified() &&
            QMessageBox::question(this, windowTitle(), tr("Drop the changes made here and read the file again?")) !=
                QMessageBox::Yes) {
            return;
        }
        m_editor->reload();
        refreshAll();
    });
    connect(close, &QPushButton::clicked, this, [this]() {
        if (m_editor->modified() && !m_readOnly) {
            const auto answer = QMessageBox::question(this, windowTitle(), tr("Save the changes made here?"),
                                                      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            if (answer == QMessageBox::Cancel) return;
            if (answer == QMessageBox::Save && !save()) return;
        }
        accept();
    });
}

void AssetServersDialog::report(const QString& error) { QMessageBox::warning(this, windowTitle(), error); }

void AssetServersDialog::refreshAll() {
    const std::string fileText = assets::toUtf8(m_editor->file());
    m_fileLabel->setText(QDir::toNativeSeparators(qs(fileText)));
    // A studio's file (USDCC_ASSETS_CONFIG) or one this user can't write: shown, Test and
    // Login working, and a copy of it can become this user's own.
    m_readOnly = !m_editor->writable();
    const bool fromStudio = !PXR_NS::TfGetenv("USDCC_ASSETS_CONFIG").empty();
    QStringList notes;
    if (!m_editor->loadProblem().empty()) {
        notes << tr("The file can't be read as it is, so it can't be edited here until it's fixed in a text "
                    "editor:\n%1")
                     .arg(qs(m_editor->loadProblem()));
    }
    if (m_readOnly) {
        notes << tr("This file can't be written here%1. Test and Login work; Save a Copy as Mine writes it as your "
                    "own assets.json.")
                     .arg(fromStudio ? tr(" (it comes from USDCC_ASSETS_CONFIG)") : QString());
        if (fromStudio) notes << tr("While USDCC_ASSETS_CONFIG is set, it still wins over your own copy.");
    } else if (fromStudio) {
        notes << tr("This file comes from USDCC_ASSETS_CONFIG.");
    }
    m_notice->setText(notes.join(QStringLiteral("\n")));
    m_notice->setVisible(!notes.isEmpty());
    m_saveCopy->setVisible(m_readOnly);
    refreshServers();
    refreshEnvironments();
    refreshWebHosts();
    refreshDefaultEnvironment();
    setEditable(!m_readOnly && m_editor->loadProblem().empty());
    refreshProblems();
}

void AssetServersDialog::setEditable(bool editable) {
    const QAbstractItemView::EditTriggers triggers =
        editable ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                       QAbstractItemView::SelectedClicked
                 : QAbstractItemView::NoEditTriggers;
    m_servers->setEditTriggers(triggers);
    m_webHosts->setEditTriggers(triggers);
    for (QWidget* widget : std::initializer_list<QWidget*>{
             m_addServer, m_removeServer, m_addMirror, m_removeMirror, m_s3Region, m_s3BucketInHost,
             m_addEnvironment, m_duplicateEnvironment, m_renameEnvironment, m_removeEnvironment, m_addName,
             m_removeName, m_stackServer, m_addToStack, m_removeFromStack, m_addWebHost, m_removeWebHost, m_httpAllow,
             m_s3Allow, m_defaultEnvironment}) {
        widget->setEnabled(editable);
    }
    m_stack->setDragDropMode(editable ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
    for (int row = 0; row < m_servers->rowCount(); ++row) {
        if (QWidget* protocol = m_servers->cellWidget(row, kProtocol)) protocol->setEnabled(editable);
    }
    m_servers->setProperty("usdccEditable", editable);
}

void AssetServersDialog::refreshServers() {
    const QString selected = selectedServer();
    const RefreshGuard guard(m_refreshing);
    m_servers->setRowCount(0);
    const bool editable = m_servers->property("usdccEditable").isValid() ? m_servers->property("usdccEditable").toBool()
                                                                         : true;
    for (const auto& [id, endpoint] : m_editor->config().endpoints) {
        const int row = m_servers->rowCount();
        m_servers->insertRow(row);
        QTableWidgetItem* idItem = textItem(qs(id));
        idItem->setData(Qt::UserRole, qs(id));
        m_servers->setItem(row, kId, idItem);
        auto* protocol = new QComboBox(m_servers);
        protocol->addItems(serverProtocols(qs(endpoint.scheme)));
        protocol->setCurrentText(qs(endpoint.scheme));
        protocol->setEnabled(editable);
        const QString rowId = qs(id);
        connect(protocol, &QComboBox::currentTextChanged, this, [this, rowId](const QString& scheme) {
            if (m_refreshing) return;
            auto found = m_editor->config().endpoints.find(rowId.toStdString());
            if (found == m_editor->config().endpoints.end()) return;
            found->second.scheme = scheme.toStdString();
            refreshServerDetails();
            refreshProblems();
        });
        m_servers->setCellWidget(row, kProtocol, protocol);
        m_servers->setItem(row, kUrl, textItem(qs(endpoint.url)));
        m_servers->setItem(row, kCredential, textItem(qs(endpoint.credential)));
        m_servers->setItem(row, kReadOnly, flagItem(endpoint.readOnly));
        m_servers->setItem(row, kOptional, flagItem(endpoint.optional));
    }
    if (!selected.isEmpty()) selectServer(selected);
    if (m_servers->currentRow() < 0 && m_servers->rowCount() > 0) m_servers->selectRow(0);
    refreshServerDetails();
}

void AssetServersDialog::serverCellChanged(int row, int column) {
    if (m_refreshing) return;
    QTableWidgetItem* idItem = m_servers->item(row, kId);
    if (!idItem) return;
    const std::string id = idItem->data(Qt::UserRole).toString().toStdString();
    if (column == kId) {
        const std::string newId = idItem->text().trimmed().toStdString();
        const std::string why = m_editor->renameServer(id, newId);
        if (!why.empty()) {
            const RefreshGuard guard(m_refreshing);
            idItem->setText(qs(id));
            return report(qs(why));
        }
        const RefreshGuard guard(m_refreshing);
        idItem->setData(Qt::UserRole, qs(newId));
        // The protocol box's connection names the old id: rows are made again, once the
        // table is done with the item that changed.
        const QString renamed = qs(newId);
        QMetaObject::invokeMethod(
            this,
            [this, renamed]() {
                refreshServers();
                selectServer(renamed);
                refreshEnvironments();
                refreshProblems();
            },
            Qt::QueuedConnection);
        return;
    }
    auto found = m_editor->config().endpoints.find(id);
    if (found == m_editor->config().endpoints.end()) return;
    assets::Endpoint& endpoint = found->second;
    QTableWidgetItem* item = m_servers->item(row, column);
    switch (column) {
    case kUrl: {
        std::string url = item->text().trimmed().toStdString();
        while (url.size() > 1 && url.back() == '/') url.pop_back();
        endpoint.url = url;
        break;
    }
    case kCredential: endpoint.credential = item->text().trimmed().toStdString(); break;
    case kReadOnly: endpoint.readOnly = item->checkState() == Qt::Checked; break;
    case kOptional: endpoint.optional = item->checkState() == Qt::Checked; break;
    default: break;
    }
    refreshProblems();
}

void AssetServersDialog::refreshServerDetails() {
    const RefreshGuard guard(m_refreshing);
    const auto found = m_editor->config().endpoints.find(selectedServer().toStdString());
    const bool any = found != m_editor->config().endpoints.end();
    m_mirrors->clear();
    if (any) {
        for (const std::string& mirror : found->second.mirrors) m_mirrors->addItem(qs(mirror));
    }
    const bool s3 = any && found->second.scheme == "s3";
    m_s3Fields->setVisible(s3);
    if (s3) {
        const auto region = found->second.options.find("region");
        m_s3Region->setText(region == found->second.options.end() ? QString() : qs(region->second));
        const auto inHost = found->second.options.find("bucketInHost");
        m_s3BucketInHost->setChecked(inHost != found->second.options.end() && inHost->second == "true");
    }
    m_test->setEnabled(any);
    m_login->setEnabled(any && !found->second.credential.empty() && found->second.credential != "aws" &&
                        found->second.credential.rfind("aws:", 0) != 0);
}

void AssetServersDialog::refreshEnvironments() {
    const QString selected = selectedEnvironment();
    const RefreshGuard guard(m_refreshing);
    m_environments->clear();
    for (const auto& [environment, schemes] : m_editor->config().environments) m_environments->addItem(qs(environment));
    if (!selected.isEmpty()) selectEnvironment(selected);
    if (m_environments->currentRow() < 0 && m_environments->count() > 0) m_environments->setCurrentRow(0);
    refreshNames();
}

void AssetServersDialog::refreshNames() {
    std::string scheme, name;
    const bool had = selectedName(&scheme, &name);
    const RefreshGuard guard(m_refreshing);
    m_names->clear();
    const auto found = m_editor->config().environments.find(selectedEnvironment().toStdString());
    if (found != m_editor->config().environments.end()) {
        for (const auto& [ofScheme, names] : found->second) {
            for (const auto& [ofName, servers] : names) {
                auto* item = new QTreeWidgetItem(m_names, {qs(ofScheme) + QStringLiteral("://") + qs(ofName)});
                item->setData(0, Qt::UserRole, qs(ofScheme));
                item->setData(0, Qt::UserRole + 1, qs(ofName));
            }
        }
    }
    if (had) selectName(qs(scheme), qs(name));
    if (!m_names->currentItem() && m_names->topLevelItemCount() > 0) m_names->setCurrentItem(m_names->topLevelItem(0));
    refreshStack();
}

void AssetServersDialog::refreshStack() {
    const RefreshGuard guard(m_refreshing);
    m_stack->clear();
    m_stackServer->clear();
    std::string scheme, name;
    if (selectedName(&scheme, &name)) {
        const auto& names = m_editor->config().environments[selectedEnvironment().toStdString()][scheme];
        const auto found = names.find(name);
        if (found != names.end()) {
            for (const std::string& server : found->second) {
                auto* item = new QListWidgetItem(qs(server), m_stack);
                const auto endpoint = m_editor->config().endpoints.find(server);
                if (endpoint != m_editor->config().endpoints.end()) {
                    item->setToolTip(qs(endpoint->second.url) +
                                     (endpoint->second.readOnly ? tr(" (read-only)") : QString()));
                }
            }
            // Only servers of the name's protocol, and not those in it already (§4).
            for (const auto& [id, endpoint] : m_editor->config().endpoints) {
                if (endpoint.scheme != scheme) continue;
                if (std::find(found->second.begin(), found->second.end(), id) != found->second.end()) continue;
                m_stackServer->addItem(qs(id));
            }
        }
    }
}

void AssetServersDialog::applyStackOrder() {
    if (m_refreshing) return;
    std::string scheme, name;
    if (!selectedName(&scheme, &name)) return;
    std::vector<std::string> servers;
    for (int row = 0; row < m_stack->count(); ++row) servers.push_back(m_stack->item(row)->text().toStdString());
    const std::string why = m_editor->setStack(selectedEnvironment().toStdString(), scheme, name, servers);
    if (!why.empty()) report(qs(why));
    refreshStack();
    refreshProblems();
}

void AssetServersDialog::refreshWebHosts() {
    const RefreshGuard guard(m_refreshing);
    m_webHosts->setRowCount(0);
    for (const auto& [host, entry] : m_editor->config().httpHosts) {
        const int row = m_webHosts->rowCount();
        m_webHosts->insertRow(row);
        QTableWidgetItem* hostItem = textItem(qs(host));
        hostItem->setData(Qt::UserRole, qs(host));
        m_webHosts->setItem(row, kHost, hostItem);
        m_webHosts->setItem(row, kHostCredential, textItem(qs(entry.credential)));
        m_webHosts->setItem(row, kInsecure, flagItem(entry.insecure));
    }
    m_httpAllow->setText(joinList(m_editor->config().httpAllow));
    m_s3Allow->setText(joinList(m_editor->config().s3Allow));
}

void AssetServersDialog::webHostCellChanged(int row, int column) {
    if (m_refreshing) return;
    QTableWidgetItem* hostItem = m_webHosts->item(row, kHost);
    if (!hostItem) return;
    auto& hosts = m_editor->config().httpHosts;
    const std::string host = hostItem->data(Qt::UserRole).toString().toStdString();
    if (column == kHost) {
        const std::string renamed = hostItem->text().trimmed().toStdString();
        if (renamed.empty() || (renamed != host && hosts.count(renamed))) {
            refreshWebHosts();
            return;
        }
        assets::HttpHost entry = hosts[host];
        hosts.erase(host);
        hosts[renamed] = entry;
        refreshWebHosts();
    } else if (column == kHostCredential) {
        hosts[host].credential = m_webHosts->item(row, column)->text().trimmed().toStdString();
    } else if (column == kInsecure) {
        hosts[host].insecure = m_webHosts->item(row, column)->checkState() == Qt::Checked;
    }
    refreshProblems();
}

void AssetServersDialog::refreshDefaultEnvironment() {
    const RefreshGuard guard(m_refreshing);
    m_defaultEnvironment->clear();
    m_defaultEnvironment->addItem(tr("(none)"), QString());
    for (const auto& [environment, schemes] : m_editor->config().environments) {
        m_defaultEnvironment->addItem(qs(environment), qs(environment));
    }
    const int index = m_defaultEnvironment->findData(qs(m_editor->config().environment));
    // One the configuration names but doesn't have stays chosen, as a problem to fix.
    if (index < 0 && !m_editor->config().environment.empty()) {
        m_defaultEnvironment->addItem(qs(m_editor->config().environment), qs(m_editor->config().environment));
        m_defaultEnvironment->setCurrentIndex(m_defaultEnvironment->count() - 1);
    } else {
        m_defaultEnvironment->setCurrentIndex(std::max(0, index));
    }
}

void AssetServersDialog::refreshProblems() {
    const std::vector<std::string> problems = m_editor->problems();
    QStringList lines;
    for (const std::string& problem : problems) lines << qs(problem);
    m_problems->setText(lines.join(QStringLiteral("\n")));
    m_problems->setVisible(!lines.isEmpty());
    // Each problem at its row: a server whose id it names.
    for (int row = 0; row < m_servers->rowCount(); ++row) {
        QTableWidgetItem* idItem = m_servers->item(row, kId);
        if (!idItem) continue;
        const std::string quoted = "'" + idItem->data(Qt::UserRole).toString().toStdString() + "'";
        QStringList mine;
        for (const std::string& problem : problems) {
            if (problem.find("endpoint " + quoted) != std::string::npos) mine << qs(problem);
        }
        const RefreshGuard guard(m_refreshing);
        for (int column = 0; column < kServerColumns; ++column) {
            QTableWidgetItem* item = m_servers->item(row, column);
            if (!item) continue;
            item->setBackground(mine.isEmpty() ? QBrush() : QBrush(usdcc::ui::theme::color(usdcc::ui::theme::Token::Error, 70)));
            item->setToolTip(mine.join(QStringLiteral("\n")));
        }
    }
    for (int row = 0; row < m_environments->count(); ++row) {
        QListWidgetItem* item = m_environments->item(row);
        const std::string quoted = "environment '" + item->text().toStdString() + "'";
        QStringList mine;
        for (const std::string& problem : problems) {
            if (problem.find(quoted) != std::string::npos) mine << qs(problem);
        }
        item->setBackground(mine.isEmpty() ? QBrush() : QBrush(usdcc::ui::theme::color(usdcc::ui::theme::Token::Error, 70)));
        item->setToolTip(mine.join(QStringLiteral("\n")));
    }
    const bool canEdit = !m_readOnly && m_editor->loadProblem().empty();
    m_save->setEnabled(canEdit && problems.empty() && m_editor->modified());
    m_saveCopy->setEnabled(problems.empty() && m_editor->loadProblem().empty());
}

QStringList AssetServersDialog::problems() const {
    return m_problems->text().isEmpty() ? QStringList() : m_problems->text().split(QLatin1Char('\n'));
}

QString AssetServersDialog::testResult() const { return m_testResult->text(); }

QString AssetServersDialog::selectedServer() const {
    const int row = m_servers->currentRow();
    if (row < 0 || !m_servers->item(row, kId)) return QString();
    return m_servers->item(row, kId)->data(Qt::UserRole).toString();
}

QString AssetServersDialog::selectedEnvironment() const {
    QListWidgetItem* item = m_environments->currentItem();
    return item ? item->text() : QString();
}

bool AssetServersDialog::selectedName(std::string* scheme, std::string* name) const {
    QTreeWidgetItem* item = m_names->currentItem();
    if (!item) return false;
    *scheme = item->data(0, Qt::UserRole).toString().toStdString();
    *name = item->data(0, Qt::UserRole + 1).toString().toStdString();
    return true;
}

void AssetServersDialog::selectServer(const QString& id) {
    for (int row = 0; row < m_servers->rowCount(); ++row) {
        QTableWidgetItem* item = m_servers->item(row, kId);
        if (item && item->data(Qt::UserRole).toString() == id) {
            m_servers->setCurrentCell(row, kId);
            m_servers->selectRow(row);
            return;
        }
    }
}

void AssetServersDialog::selectEnvironment(const QString& environment) {
    const QList<QListWidgetItem*> found = m_environments->findItems(environment, Qt::MatchExactly);
    if (!found.isEmpty()) m_environments->setCurrentItem(found.front());
}

void AssetServersDialog::selectName(const QString& scheme, const QString& name) {
    for (int i = 0; i < m_names->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_names->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == scheme && item->data(0, Qt::UserRole + 1).toString() == name) {
            m_names->setCurrentItem(item);
            return;
        }
    }
}

void AssetServersDialog::testServer(const QString& id) {
    if (!id.isEmpty()) selectServer(id);
    const auto found = m_editor->config().endpoints.find(selectedServer().toStdString());
    if (found == m_editor->config().endpoints.end()) return;
    // What the table says now, unsaved.
    const assets::Endpoint endpoint = found->second;
    const int generation = ++m_testGeneration;
    m_testResult->setText(tr("%1: asking %2...").arg(qs(endpoint.id), qs(endpoint.url)));
    QPointer<AssetServersDialog> self(this);
    QThreadPool::globalInstance()->start([self, endpoint, generation]() {
        assets::Cancel cancel;
        const assets::EndpointTest test = assets::testEndpoint(endpoint, cancel);
        const QString summary = qs(test.summary);
        QMetaObject::invokeMethod(
            qApp,
            [self, summary, generation]() {
                if (!self || generation != self->m_testGeneration) return;
                self->m_testResult->setText(summary);
            },
            Qt::QueuedConnection);
    });
}

bool AssetServersDialog::save(bool overwriteChanged) {
    if (m_readOnly) return false;
    if (!overwriteChanged && m_editor->changedOnDisk()) {
        QMessageBox ask(QMessageBox::Question, windowTitle(),
                        tr("%1 changed on disk since it was read here: someone else, or a text editor.")
                            .arg(QDir::toNativeSeparators(qs(assets::toUtf8(m_editor->file())))),
                        QMessageBox::NoButton, this);
        QPushButton* overwrite = ask.addButton(tr("Overwrite It"), QMessageBox::DestructiveRole);
        QPushButton* reload = ask.addButton(tr("Reload It"), QMessageBox::AcceptRole);
        ask.addButton(QMessageBox::Cancel);
        ask.exec();
        if (ask.clickedButton() == reload) {
            m_editor->reload();
            refreshAll();
            return false;
        }
        if (ask.clickedButton() != overwrite) return false;
    }
    const assets::Config before = assets::defaultConfig();
    const assets::Result<bool> written = m_editor->save(true);
    if (!written.ok()) {
        report(tr("It couldn't be saved: %1").arg(qs(written.message)));
        return false;
    }
    // Applies at once, as RefreshContext does: read again, and what servers answered forgotten.
    assets::reloadDefaultConfig();
    // Open stages keep the servers they were opened with: their contexts hold the stacks.
    const std::map<std::string, std::string> was = environmentFingerprints(before);
    const std::map<std::string, std::string> now = environmentFingerprints(m_editor->config());
    m_keptStages = 0;
    if (const StageManager* stages = stageManagerPointer()) {
        for (const StageRefPtr& stage : stages->stages()) {
            if (!stage) continue;
            const std::string environment = stageAssetEnvironment(stage->usdStage());
            if (environment.empty()) continue;
            const auto a = was.find(environment), b = now.find(environment);
            const std::string oldText = a == was.end() ? std::string() : a->second;
            const std::string newText = b == now.end() ? std::string() : b->second;
            if (oldText != newText) ++m_keptStages;
        }
    }
    QString message = tr("Saved.");
    if (m_keptStages > 0) {
        message += QStringLiteral(" ") + tr("%n open stage(s) in a changed environment keep the servers they were "
                                            "opened with; reopening them uses the new ones.",
                                            nullptr, m_keptStages);
    }
    m_problems->setVisible(false);
    m_testResult->setText(message);
    refreshProblems();
    Q_EMIT saved();
    return true;
}

bool AssetServersDialog::saveCopyAsMine(const std::filesystem::path& file) {
    const std::filesystem::path target = file.empty() ? assets::userDataFolder() / "assets.json" : file;
    if (std::filesystem::exists(target) &&
        QMessageBox::question(this, windowTitle(),
                              tr("%1 exists. Replace it with this configuration?")
                                  .arg(QDir::toNativeSeparators(qs(assets::toUtf8(target))))) != QMessageBox::Yes) {
        return false;
    }
    const assets::Result<bool> written = m_editor->saveAs(target);
    if (!written.ok()) {
        report(tr("It couldn't be saved: %1").arg(qs(written.message)));
        return false;
    }
    assets::reloadDefaultConfig();
    refreshAll();
    Q_EMIT saved();
    return true;
}

bool showAssetServersEditor(QWidget* parent) {
    AssetServersDialog dialog(parent);
    bool saved = false;
    QObject::connect(&dialog, &AssetServersDialog::saved, &dialog, [&saved]() { saved = true; });
    dialog.exec();
    return saved;
}

}  // namespace usdcc::usd
