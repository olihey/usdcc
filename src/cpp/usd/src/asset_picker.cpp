#include "usdcc/usd/asset_picker.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/transport.h"
#include "usdcc/assets/uri.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/asset_servers.h"
#include "usdcc/usd/asset_settings.h"
#include "usdcc/usd/asset_sources.h"
#include "usdcc/usd/properties/property_editor.h"

#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace usdcc::usd {

namespace {

constexpr auto kFavoritesKey = "AssetPicker/favorites";
constexpr auto kRecentKey = "AssetPicker/recentFolders";
constexpr int kRecentCount = 10;
constexpr int kPathRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;
const QString kFolder = QStringLiteral("folder");
const QString kFile = QStringLiteral("file");
const QString kFavorite = QStringLiteral("favorite");

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

bool isRemote(const QString& path) { return !path.isEmpty() && !isLocalAssetPath(path.toStdString()); }

QString schemeOf(const QString& location) {
    const int colon = location.indexOf(QStringLiteral("://"));
    return colon <= 1 ? QStringLiteral("file") : location.left(colon).toLower();
}

QString restOf(const QString& location) {
    const int colon = location.indexOf(QStringLiteral("://"));
    return colon <= 1 ? location : location.mid(colon + 3);
}

// "file:///C:/a/b.usd" is the path C:/a/b.usd: OpenUSD has no file scheme (§3.2).
QString withoutFileScheme(const QString& text) {
    if (text.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) return QUrl(text).toLocalFile();
    return text;
}

bool samePath(const QString& a, const QString& b) {
    return isRemote(a) || isRemote(b) ? a == b : a.compare(b, kPathCase) == 0;
}

std::vector<AssetFavorite> readFavorites() {
    std::vector<AssetFavorite> favorites;
    const QJsonArray array = QJsonDocument::fromJson(usdcc::ui::appSettings().value(kFavoritesKey).toString().toUtf8()).array();
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const QString path = object.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) continue;
        favorites.push_back({object.value(QStringLiteral("label")).toString(), path});
    }
    return favorites;
}

void writeFavorites(const std::vector<AssetFavorite>& favorites) {
    QJsonArray array;
    for (const AssetFavorite& favorite : favorites) {
        array.append(QJsonObject{{QStringLiteral("label"), favorite.label}, {QStringLiteral("path"), favorite.path}});
    }
    QSettings settings = usdcc::ui::appSettings();
    if (array.isEmpty()) {
        settings.remove(kFavoritesKey);
    } else {
        settings.setValue(kFavoritesKey, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    }
}

int findFavorite(const std::vector<AssetFavorite>& favorites, const QString& folder) {
    for (size_t i = 0; i < favorites.size(); ++i) {
        if (samePath(favorites[i].path, folder)) return static_cast<int>(i);
    }
    return -1;
}

// "hero" for webdav://studio/chars/hero/ and C:/show/hero; the server for webdav://studio/.
QString lastPart(const QString& folder) {
    QString path = folder;
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1) path.chop(1);
    if (isRemote(path) && !restOf(path).contains(QLatin1Char('/'))) return restOf(path);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString name = slash < 0 ? path : path.mid(slash + 1);
    return name.isEmpty() ? path : name;
}

QString parentOf(const QString& folder) {
    QString path = folder;
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1) path.chop(1);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

void addRecentFolder(const QString& folder) {
    QStringList folders = recentAssetFolders();
    folders.erase(std::remove_if(folders.begin(), folders.end(), [&folder](const QString& each) { return samePath(each, folder); }),
                  folders.end());
    folders.prepend(folder);
    while (folders.size() > kRecentCount) folders.removeLast();
    usdcc::ui::appSettings().setValue(kRecentKey, folders);
}

QStringList withAllFiles(QStringList filters) {
    const bool hasAll = std::any_of(filters.begin(), filters.end(),
                                    [](const QString& filter) { return filter.contains(QStringLiteral("(*)")); });
    if (!hasAll) filters << QObject::tr("All Files (*)");
    return filters;
}

QString sizeText(quint64 bytes) {
    if (bytes < 1024) return QObject::tr("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QObject::tr("%1 KB").arg((bytes + 512) / 1024);
    return QObject::tr("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

usdcc::assets::Context assetsContext(const PXR_NS::ArResolverContext& context) {
    if (const usdcc::assets::Context* own = context.Get<usdcc::assets::Context>()) return *own;
    return *usdcc::assets::defaultContext();
}

// A favourite on a server name the environment doesn't have can't be opened: known without
// asking anyone (§3.1). Why, or empty when it can be.
QString unreachableBecause(const QString& path, const PXR_NS::ArResolverContext& context) {
    if (!isRemote(path)) return {};
    usdcc::assets::Uri uri;
    if (!usdcc::assets::parseUri(path.toStdString(), &uri) || !uri.stackId.empty()) return {};
    const usdcc::assets::Context assets = assetsContext(context);
    if (assets.stack(uri.scheme, uri.name)) return {};
    const usdcc::assets::AddressParser parser = usdcc::assets::addressParserFor(uri.scheme);
    usdcc::assets::Endpoint endpoint;
    std::string rest;
    std::string normalized;
    if (parser && parser(path.toStdString(), &endpoint, &rest, &normalized)) return {};
    return QObject::tr("The environment '%1' has no server named '%2'")
        .arg(QString::fromStdString(assets.environment()), QString::fromStdString(uri.name));
}

}  // namespace

// ---- Favourites and recent folders -----------------------------------------------------------

QString normalizedAssetFolder(const QString& folder) {
    QString path = withoutFileScheme(folder.trimmed());
    if (path.isEmpty()) return {};
    if (isRemote(path)) {
        path = QString::fromStdString(displayAssetPath(path.toStdString()));
        if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
        return path;
    }
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    return path;
}

std::vector<AssetFavorite> assetFavorites() { return readFavorites(); }

bool isAssetFavorite(const QString& folder) {
    return findFavorite(readFavorites(), normalizedAssetFolder(folder)) >= 0;
}

bool addAssetFavorite(const QString& folder, const QString& label) {
    const QString path = normalizedAssetFolder(folder);
    if (path.isEmpty()) return false;
    std::vector<AssetFavorite> favorites = readFavorites();
    if (findFavorite(favorites, path) >= 0) return false;
    QString name = label.trimmed();
    if (name.isEmpty()) {
        name = lastPart(path);
        const bool taken = std::any_of(favorites.begin(), favorites.end(),
                                       [&name](const AssetFavorite& each) { return each.label == name; });
        if (taken && !parentOf(path).isEmpty()) name = QStringLiteral("%1 (%2)").arg(name, lastPart(parentOf(path)));
    }
    favorites.push_back({name, path});
    writeFavorites(favorites);
    return true;
}

bool removeAssetFavorite(const QString& folder) {
    std::vector<AssetFavorite> favorites = readFavorites();
    const int index = findFavorite(favorites, normalizedAssetFolder(folder));
    if (index < 0) return false;
    favorites.erase(favorites.begin() + index);
    writeFavorites(favorites);
    return true;
}

bool renameAssetFavorite(const QString& folder, const QString& label) {
    std::vector<AssetFavorite> favorites = readFavorites();
    const int index = findFavorite(favorites, normalizedAssetFolder(folder));
    if (index < 0 || label.trimmed().isEmpty()) return false;
    favorites[index].label = label.trimmed();
    writeFavorites(favorites);
    return true;
}

bool moveAssetFavorite(const QString& folder, int delta) {
    std::vector<AssetFavorite> favorites = readFavorites();
    const int index = findFavorite(favorites, normalizedAssetFolder(folder));
    const int to = index + delta;
    if (index < 0 || to < 0 || to >= static_cast<int>(favorites.size())) return false;
    std::swap(favorites[index], favorites[to]);
    writeFavorites(favorites);
    return true;
}

QStringList recentAssetFolders() { return usdcc::ui::appSettings().value(kRecentKey).toStringList(); }

QStringList assetPickerSchemes() {
    QStringList schemes{QStringLiteral("file")};
    const std::vector<std::string> browsable = AssetSourceRegistry::instance().browsableSchemes();
    for (const std::string& scheme : browsable) schemes << QString::fromStdString(scheme);
    for (const std::string& scheme : registeredUriSchemes()) {
        if (std::find(browsable.begin(), browsable.end(), scheme) == browsable.end()) schemes << QString::fromStdString(scheme);
    }
    schemes.removeDuplicates();
    return schemes;
}

bool assetPickerAvailable() { return !AssetSourceRegistry::instance().browsableSchemes().empty(); }

QString assetPathForChoice(const QString& chosen, const PXR_NS::SdfLayerHandle& anchor, bool relative) {
    if (chosen.isEmpty() || !anchor || !relative) return chosen;
    if (isRemote(chosen)) {
        const std::string path = relativeAssetPath(chosen.toStdString(), anchor->GetIdentifier());
        return path.empty() ? chosen : QString::fromStdString(path);
    }
    if (isRemote(QString::fromStdString(anchor->GetIdentifier()))) return chosen;
    return properties::assetPathForFile(chosen, properties::layerFolder(anchor));
}

QString pickAsset(QWidget* parent, const AssetPickerOptions& options) {
    if (!assetPickerAvailable()) {
        // Nothing to browse besides the disk: the system dialog, as before (§3).
        const QString start = isRemote(options.start) ? QString() : options.start;
        const QString file =
            QFileDialog::getOpenFileName(parent, options.title, start, withAllFiles(options.filters).join(QStringLiteral(";;")));
        if (file.isEmpty()) return {};
        return assetPathForChoice(QDir::cleanPath(file), options.anchorLayer, true);
    }
    AssetPickerDialog dialog(options, parent);
    if (dialog.exec() != QDialog::Accepted) return {};
    return dialog.result();
}

// ---- The dialog -------------------------------------------------------------------------------

struct AssetPickerDialog::Listed {
    QString name;
    bool folder = false;
    quint64 size = 0;
    double modified = 0;
    QString origin;
    QString path;  // set when the entry isn't simply the location's child: a drive, a root
};

AssetPickerDialog::AssetPickerDialog(const AssetPickerOptions& options, QWidget* parent)
    : QDialog(parent), m_options(options) {
    setWindowTitle(options.title.isEmpty() ? tr("Choose Asset") : options.title);
    setObjectName(QStringLiteral("assetPickerDialog"));
    auto* layout = new QVBoxLayout(this);

    // The path bar: the scheme, the rest of the path, favourite, up.
    auto* bar = new QHBoxLayout();
    m_scheme = new QComboBox(this);
    m_scheme->setObjectName(QStringLiteral("assetScheme"));
    for (const QString& scheme : assetPickerSchemes()) {
        m_scheme->addItem(scheme + QStringLiteral("://"), scheme);
        const std::string type = scheme == QStringLiteral("file") ? std::string() : resolverTypeForScheme(scheme.toStdString());
        m_scheme->setItemData(m_scheme->count() - 1,
                              scheme == QStringLiteral("file") ? tr("Files on disk")
                              : AssetSourceRegistry::instance().forScheme(scheme.toStdString())
                                  ? tr("Browsable, through %1").arg(QString::fromStdString(type.empty() ? scheme.toStdString() : type))
                                  : tr("Not browsable: type the rest of the path. Through %1")
                                        .arg(QString::fromStdString(type.empty() ? scheme.toStdString() : type)),
                              Qt::ToolTipRole);
    }
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("assetPath"));
    m_path->setPlaceholderText(tr("A folder or file; a whole URI switches the scheme"));
    m_favorite = new QToolButton(this);
    m_favorite->setObjectName(QStringLiteral("assetFavorite"));
    m_up = new QToolButton(this);
    m_up->setText(tr("Up"));
    m_up->setToolTip(tr("The folder above"));
    bar->addWidget(m_scheme);
    bar->addWidget(m_path, 1);
    bar->addWidget(m_favorite);
    bar->addWidget(m_up);
    layout->addLayout(bar);

    auto* splitter = new QSplitter(this);
    m_places = new QTreeWidget(splitter);
    m_places->setObjectName(QStringLiteral("assetPlaces"));
    m_places->setHeaderHidden(true);
    m_places->setContextMenuPolicy(Qt::CustomContextMenu);
    m_entries = new QTreeWidget(splitter);
    m_entries->setObjectName(QStringLiteral("assetEntries"));
    m_entries->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("From")});
    m_entries->setRootIsDecorated(false);
    m_entries->setContextMenuPolicy(Qt::CustomContextMenu);
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->header()->setStretchLastSection(false);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({usdcc::ui::scaled::pixels(200), usdcc::ui::scaled::pixels(560)});
    layout->addWidget(splitter, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("assetStatus"));
    m_status->setEnabled(false);
    layout->addWidget(m_status);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel(tr("File name:"), this));
    m_fileName = new QLineEdit(this);
    m_fileName->setObjectName(QStringLiteral("assetFileName"));
    nameRow->addWidget(m_fileName, 1);
    m_filter = new QComboBox(this);
    m_filter->setObjectName(QStringLiteral("assetFilter"));
    m_filter->addItems(withAllFiles(options.filters));
    nameRow->addWidget(m_filter);
    layout->addLayout(nameRow);

    auto* buttonsRow = new QHBoxLayout();
    if (options.anchorLayer) {
        buttonsRow->addWidget(new QLabel(tr("Path:"), this));
        m_relative = new QRadioButton(tr("Relative"), this);
        m_relative->setObjectName(QStringLiteral("assetRelative"));
        m_absolute = new QRadioButton(tr("Absolute"), this);
        m_relative->setToolTip(tr("Relative to the layer it's written in, when that can be"));
        m_relative->setChecked(true);
        auto* group = new QButtonGroup(this);
        group->addButton(m_relative);
        group->addButton(m_absolute);
        buttonsRow->addWidget(m_relative);
        buttonsRow->addWidget(m_absolute);
    }
    buttonsRow->addStretch(1);
    auto* system = new QPushButton(tr("System Dialog..."), this);
    system->setToolTip(tr("Choose a file on disk with the system's own dialog"));
    system->setAutoDefault(false);
    buttonsRow->addWidget(system);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    buttonsRow->addWidget(buttons);
    layout->addLayout(buttonsRow);

    connect(buttons, &QDialogButtonBox::accepted, this, &AssetPickerDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(system, &QPushButton::clicked, this, &AssetPickerDialog::systemDialog);
    connect(m_scheme, &QComboBox::activated, this, &AssetPickerDialog::schemeChosen);
    connect(m_up, &QToolButton::clicked, this, &AssetPickerDialog::goUp);
    connect(m_favorite, &QToolButton::clicked, this, &AssetPickerDialog::toggleFavorite);
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this]() { navigate(m_location); });
    connect(m_places, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const QString path = item->data(0, kPathRole).toString();
        const QString kind = item->data(0, kKindRole).toString();
        // Headings aren't places, but a server heading is its scheme's roots.
        if (item->isDisabled() || kind == QStringLiteral("favorites") || (kind.isEmpty() && path.isEmpty())) return;
        navigate(path);
    });
    connect(m_places, &QTreeWidget::customContextMenuRequested, this, &AssetPickerDialog::placesMenu);
    connect(m_entries, &QTreeWidget::customContextMenuRequested, this, &AssetPickerDialog::entriesMenu);
    connect(m_entries, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString path = item->data(0, kPathRole).toString();
        if (item->data(0, kKindRole).toString() == kFolder) {
            navigate(path);
        } else if (item->data(0, kKindRole).toString() == kFile) {
            acceptCandidate(path, true);
        }
    });
    connect(m_entries, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, kKindRole).toString() == kFile) m_fileName->setText(item->text(0));
    });

    buildPlaces();
    resize(usdcc::ui::scaled::size(860, 520));

    // Where to start: the given folder, or a given file's, with its name ready; else the last.
    QString start = withoutFileScheme(options.start.trimmed());
    if (!start.isEmpty()) {
        if (isRemote(start)) {
            start = QString::fromStdString(displayAssetPath(start.toStdString()));
            if (!start.endsWith(QLatin1Char('/'))) {
                m_fileName->setText(start.mid(start.lastIndexOf(QLatin1Char('/')) + 1));
                start = start.left(start.lastIndexOf(QLatin1Char('/')) + 1);
            }
        } else if (QFileInfo(start).isFile()) {
            m_fileName->setText(QFileInfo(start).fileName());
            start = QFileInfo(start).absolutePath();
        }
    } else {
        const QStringList recent = recentAssetFolders();
        start = recent.isEmpty() ? QDir::homePath() : recent.front();
    }
    navigate(start);
}

AssetPickerDialog::~AssetPickerDialog() {
    if (m_cancel) m_cancel->cancel();
}

QString AssetPickerDialog::result() const {
    return assetPathForChoice(m_chosen, m_options.anchorLayer, m_relative && m_relative->isChecked());
}

QStringList AssetPickerDialog::patterns() const {
    const QString filter = m_filter->currentText();
    const int open = filter.lastIndexOf(QLatin1Char('('));
    const int close = filter.lastIndexOf(QLatin1Char(')'));
    if (open < 0 || close <= open) return {QStringLiteral("*")};
    return filter.mid(open + 1, close - open - 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

void AssetPickerDialog::setStatus(const QString& text) { m_status->setText(text); }

QString AssetPickerDialog::childOf(const QString& name) const {
    if (isRemote(m_location) || m_location.endsWith(QStringLiteral("://"))) {
        return m_location + (m_location.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/")) + name;
    }
    if (m_location.isEmpty()) return {};
    return QDir::cleanPath(QDir(m_location).filePath(name));
}

void AssetPickerDialog::navigate(const QString& target) {
    QString location = withoutFileScheme(target.trimmed());
    const QString scheme = schemeOf(location);
    if (scheme != QStringLiteral("file") && !assetPickerSchemes().contains(scheme)) {
        setStatus(tr("No resolver handles %1://").arg(scheme));
        return;
    }
    const bool remote = scheme != QStringLiteral("file");
    const bool root = remote && restOf(location).isEmpty();
    if (remote && !root) {
        location = QString::fromStdString(displayAssetPath(location.toStdString()));
        if (!location.endsWith(QLatin1Char('/'))) location += QLatin1Char('/');
        // A server the user goes to may be contacted, as a stage they open (§3.1).
        usdcc::assets::allowHostOfUrl(location.toStdString());
    } else if (!remote && !location.isEmpty()) {
        location = QDir::cleanPath(QFileInfo(location).absoluteFilePath());
    }
    if (root) location = scheme + QStringLiteral("://");

    if (m_cancel) m_cancel->cancel();
    m_cancel.reset();
    ++m_generation;
    m_listing = false;
    m_location = location;

    m_scheme->setCurrentIndex(std::max(0, m_scheme->findData(scheme)));
    m_path->setText(remote ? restOf(location) : location);
    m_up->setEnabled(!location.isEmpty() && !root);
    updateFavoriteButton();

    if (!remote && location.isEmpty()) {
        // This PC: its drives, and the home folder.
        std::vector<Listed> entries;
        for (const QFileInfo& drive : QDir::drives()) {
            entries.push_back({QDir::toNativeSeparators(drive.absoluteFilePath()), true, 0, 0, {}, drive.absoluteFilePath()});
        }
        entries.push_back({tr("Home"), true, 0, 0, {}, QDir::cleanPath(QDir::homePath())});
        showEntries(entries, {});
        return;
    }
    const std::shared_ptr<AssetSource> source =
        remote ? AssetSourceRegistry::instance().forScheme(scheme.toStdString()) : nullptr;
    if (root) {
        std::vector<Listed> entries;
        if (source) {
            for (const AssetRoot& each : source->roots(scheme.toStdString(), m_options.context)) {
                entries.push_back({QString::fromStdString(each.label), true, 0, 0, {}, QString::fromStdString(each.uri)});
            }
        }
        showEntries(entries, {});
        setStatus(source ? (entries.empty() ? tr("No servers of %1:// in the asset configuration: type a server's "
                                                 "address, such as %1://192.168.1.5:5000/")
                                                  .arg(scheme)
                                            : QString())
                         : tr("%1:// can't be browsed: type the rest of the file's path above").arg(scheme));
        return;
    }
    if (remote && !source) {
        showEntries({}, {});
        setStatus(tr("%1:// can't be browsed: type the file's name below, or its whole path above").arg(scheme));
        return;
    }
    listOnWorker(location);
}

void AssetPickerDialog::listOnWorker(const QString& location) {
    const QString scheme = schemeOf(location);
    const std::shared_ptr<AssetSource> source =
        scheme == QStringLiteral("file") ? nullptr : AssetSourceRegistry::instance().forScheme(scheme.toStdString());
    m_cancel = std::make_shared<usdcc::assets::Cancel>();
    const int generation = m_generation;
    m_listing = true;
    m_entries->clear();
    m_listedFiles.clear();
    setStatus(tr("Listing %1...").arg(displayPath(location)));

    QPointer<AssetPickerDialog> guard(this);
    const std::shared_ptr<usdcc::assets::Cancel> cancel = m_cancel;
    const PXR_NS::ArResolverContext context = m_options.context;
    // Detached: an offline share or a slow server mustn't hold up the dialog, or its closing.
    std::thread([guard, generation, location, source, cancel, context]() {
        std::vector<Listed> entries;
        QString error;
        try {
            if (!source) {
                const QDir folder(location);
                if (!folder.exists()) {
                    error = tr("This folder isn't there");
                } else {
                    for (const QFileInfo& info : folder.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                        entries.push_back({info.fileName(), info.isDir(), quint64(info.size()),
                                           double(info.lastModified().toSecsSinceEpoch()), {}, {}});
                    }
                }
            } else {
                for (const AssetListEntry& each : source->list(location.toStdString(), context, *cancel)) {
                    entries.push_back({QString::fromStdString(each.name), each.isFolder, each.size, each.modified,
                                       QString::fromStdString(each.origin), {}});
                }
            }
        } catch (const std::exception& failure) {
            error = QString::fromStdString(failure.what());
        }
        if (cancel->cancelled()) return;
        QMetaObject::invokeMethod(
            qApp,
            [guard, generation, entries = std::move(entries), error]() {
                if (!guard || guard->m_generation != generation) return;
                guard->m_listing = false;
                guard->showEntries(entries, error);
            },
            Qt::QueuedConnection);
    }).detach();
}

void AssetPickerDialog::showEntries(const std::vector<Listed>& entries, const QString& error) {
    m_entries->clear();
    m_listedFiles.clear();
    if (!error.isEmpty()) {
        auto* item = new QTreeWidgetItem(m_entries, {error});
        item->setDisabled(true);
        setStatus(tr("Couldn't list %1: %2").arg(displayPath(m_location), error));
        return;
    }
    QList<QRegularExpression> filters;
    for (const QString& pattern : patterns()) {
        filters << QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern),
                                      QRegularExpression::CaseInsensitiveOption);
    }
    std::vector<Listed> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const Listed& a, const Listed& b) {
        if (a.folder != b.folder) return a.folder;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    int folders = 0;
    int files = 0;
    bool origins = false;
    for (const Listed& entry : sorted) {
        if (!entry.folder &&
            !std::any_of(filters.begin(), filters.end(), [&entry](const QRegularExpression& re) { return re.match(entry.name).hasMatch(); })) {
            continue;
        }
        const QString path = !entry.path.isEmpty() ? entry.path
                             : entry.folder && (isRemote(m_location)) ? childOf(entry.name) + QStringLiteral("/")
                                                                       : childOf(entry.name);
        auto* item = new QTreeWidgetItem(m_entries);
        item->setText(0, entry.name);
        item->setIcon(0, entry.folder ? folderIcon : fileIcon);
        if (!entry.folder) item->setText(1, sizeText(entry.size));
        if (entry.modified > 0) {
            item->setText(2, QDateTime::fromSecsSinceEpoch(qint64(entry.modified)).toString(QStringLiteral("yyyy-MM-dd hh:mm")));
        }
        item->setText(3, entry.origin);
        item->setData(0, kPathRole, path);
        item->setData(0, kKindRole, entry.folder ? kFolder : kFile);
        item->setToolTip(0, displayPath(path));
        origins = origins || !entry.origin.isEmpty();
        if (entry.folder) {
            ++folders;
        } else {
            ++files;
            m_listedFiles << path;
        }
    }
    m_entries->setColumnHidden(3, !origins);
    if (folders + files == 0) {
        setStatus(tr("Nothing here"));
    } else {
        setStatus(tr("%1 folders, %2 files").arg(folders).arg(files));
    }
    // A file named in the field is selected when it's here.
    for (int i = 0; i < m_entries->topLevelItemCount(); ++i) {
        if (m_entries->topLevelItem(i)->text(0) == m_fileName->text()) m_entries->setCurrentItem(m_entries->topLevelItem(i));
    }
}

void AssetPickerDialog::buildPlaces() {
    m_places->clear();
    const auto group = [this](const QString& title) {
        auto* item = new QTreeWidgetItem(m_places, {title});
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        item->setFlags(Qt::ItemIsEnabled);
        return item;
    };
    const auto place = [](QTreeWidgetItem* parent, const QString& label, const QString& path, const QString& kind) {
        auto* item = new QTreeWidgetItem(parent, {label});
        item->setData(0, kPathRole, path);
        item->setData(0, kKindRole, kind);
        item->setToolTip(0, displayPath(path));
        return item;
    };
    const auto hint = [](QTreeWidgetItem* parent, const QString& text) {
        auto* item = new QTreeWidgetItem(parent, {text});
        item->setDisabled(true);
        return item;
    };

    QTreeWidgetItem* favorites = group(tr("Favourites"));
    favorites->setData(0, kKindRole, QStringLiteral("favorites"));
    for (const AssetFavorite& favorite : readFavorites()) {
        QTreeWidgetItem* item = place(favorites, favorite.label, favorite.path, kFavorite);
        const QString why = unreachableBecause(favorite.path, m_options.context);
        if (!why.isEmpty()) {
            item->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
            item->setToolTip(0, displayPath(favorite.path) + QStringLiteral("\n") + why);
        }
    }
    if (favorites->childCount() == 0) hint(favorites, tr("\xE2\x98\x86 adds the folder shown"));

    QTreeWidgetItem* computer = group(tr("This PC"));
    place(computer, tr("Drives"), QString(), kFolder);
    place(computer, tr("Home"), QDir::cleanPath(QDir::homePath()), kFolder);

    const QStringList recent = recentAssetFolders();
    if (!recent.isEmpty()) {
        QTreeWidgetItem* recentGroup = group(tr("Recent"));
        for (const QString& folder : recent) place(recentGroup, lastPart(folder), folder, kFolder);
    }

    for (const std::shared_ptr<AssetSource>& source : AssetSourceRegistry::instance().sources()) {
        for (const std::string& scheme : source->schemes()) {
            const std::vector<AssetRoot> roots = source->roots(scheme, m_options.context);
            // A scheme with no roots (webdavs:// beside webdav://) is still in the dropdown.
            if (roots.empty()) continue;
            QTreeWidgetItem* serverGroup = group(QString::fromStdString(scheme) + QStringLiteral("://"));
            serverGroup->setData(0, kPathRole, QString::fromStdString(scheme) + QStringLiteral("://"));
            for (const AssetRoot& root : roots) {
                place(serverGroup, QString::fromStdString(root.label), QString::fromStdString(root.uri), kFolder);
            }
        }
    }
    m_places->expandAll();
}

void AssetPickerDialog::schemeChosen(int index) {
    const QString scheme = m_scheme->itemData(index).toString();
    navigate(scheme == QStringLiteral("file") ? QDir::homePath() : scheme + QStringLiteral("://"));
}

void AssetPickerDialog::goUp() {
    if (isRemote(m_location)) {
        QString path = m_location;
        if (path.endsWith(QLatin1Char('/'))) path.chop(1);
        if (!restOf(path).contains(QLatin1Char('/'))) {
            navigate(schemeOf(path) + QStringLiteral("://"));
        } else {
            navigate(path.left(path.lastIndexOf(QLatin1Char('/')) + 1));
        }
        return;
    }
    if (m_location.isEmpty()) return;
    QDir folder(m_location);
    navigate(folder.isRoot() || !folder.cdUp() ? QString() : folder.absolutePath());
}

void AssetPickerDialog::updateFavoriteButton() {
    // A folder: on disk, or on a server below its scheme's roots.
    const bool folder = !m_location.isEmpty() && !m_location.endsWith(QStringLiteral("://"));
    m_favorite->setEnabled(folder);
    const bool favorite = folder && isAssetFavorite(m_location);
    m_favorite->setText(favorite ? QStringLiteral("\xE2\x98\x85") : QStringLiteral("\xE2\x98\x86"));
    m_favorite->setToolTip(favorite ? tr("Remove this folder from Favourites") : tr("Add this folder to Favourites"));
}

void AssetPickerDialog::toggleFavorite() {
    if (isAssetFavorite(m_location)) {
        removeAssetFavorite(m_location);
    } else {
        addAssetFavorite(m_location);
    }
    buildPlaces();
    updateFavoriteButton();
}

void AssetPickerDialog::pathEntered() {
    const QString text = m_path->text().trimmed();
    if (text.isEmpty()) return;
    const QString scheme = m_scheme->currentData().toString();
    QString candidate = withoutFileScheme(text);
    if (!candidate.contains(QStringLiteral("://")) && scheme != QStringLiteral("file")) {
        candidate = scheme + QStringLiteral("://") + text;
    }
    const QString typed = schemeOf(candidate);
    if (typed != QStringLiteral("file") && !assetPickerSchemes().contains(typed)) {
        setStatus(tr("No resolver handles %1://").arg(typed));
        return;
    }
    if (typed == QStringLiteral("file")) {
        if (QFileInfo(candidate).isFile()) {
            acceptCandidate(candidate, false);
        } else {
            navigate(candidate);
        }
        return;
    }
    // What can't be browsed is a file to open; what can is a folder to go to.
    if (!AssetSourceRegistry::instance().forScheme(typed.toStdString())) {
        acceptCandidate(candidate, false);
    } else {
        navigate(candidate);
    }
}

bool AssetPickerDialog::acceptCandidate(const QString& candidate, bool listed) {
    QString chosen;
    QString folder;
    if (!isRemote(candidate)) {
        const QFileInfo info(candidate);
        if (info.isDir()) {
            navigate(candidate);
            return false;
        }
        if (!info.isFile()) {
            setStatus(tr("Not found: %1").arg(QDir::toNativeSeparators(candidate)));
            return false;
        }
        chosen = QDir::cleanPath(info.absoluteFilePath());
        folder = QDir::cleanPath(info.absolutePath());
    } else {
        const std::string uri = candidate.toStdString();
        if (!listed) {
            // Typed: checked through the stage's environment before it's taken.
            usdcc::assets::allowHostOfUrl(uri);
            const PXR_NS::ArResolverContextBinder binder(m_options.context);
            if (PXR_NS::ArGetResolver().Resolve(uri).empty()) {
                setStatus(tr("Not found, or the server didn't answer: %1 (the Log says which)").arg(candidate));
                return false;
            }
        }
        // One spelling of it, when OpenUSD has a resolver for the scheme to ask.
        const std::vector<std::string> registered = registeredUriSchemes();
        const bool resolvable =
            std::find(registered.begin(), registered.end(), schemeOf(candidate).toStdString()) != registered.end();
        chosen = QString::fromStdString(displayAssetPath(resolvable ? PXR_NS::ArGetResolver().CreateIdentifier(uri) : uri));
        folder = chosen.left(chosen.lastIndexOf(QLatin1Char('/')) + 1);
    }
    m_chosen = chosen;
    addRecentFolder(folder);
    QDialog::accept();
    return true;
}

void AssetPickerDialog::accept() {
    // Enter in the path bar goes there, rather than opening.
    if (m_path->hasFocus()) {
        pathEntered();
        return;
    }
    const QString text = m_fileName->text().trimmed();
    QTreeWidgetItem* current = m_entries->currentItem();
    if (text.isEmpty()) {
        if (!current) return;
        const QString path = current->data(0, kPathRole).toString();
        if (current->data(0, kKindRole).toString() == kFolder) {
            navigate(path);
        } else if (current->data(0, kKindRole).toString() == kFile) {
            acceptCandidate(path, true);
        }
        return;
    }
    QString candidate = withoutFileScheme(text);
    if (!candidate.contains(QStringLiteral("://")) && !QDir::isAbsolutePath(candidate)) {
        if (m_location.isEmpty() || m_location.endsWith(QStringLiteral("://"))) {
            setStatus(tr("Go to a folder first, or type a whole path"));
            return;
        }
        candidate = childOf(candidate);
    }
    const bool listed = std::any_of(m_listedFiles.begin(), m_listedFiles.end(),
                                    [&candidate](const QString& each) { return samePath(each, candidate); });
    acceptCandidate(candidate, listed);
}

void AssetPickerDialog::systemDialog() {
    const QString start = isRemote(m_location) || m_location.isEmpty() ? QDir::homePath() : m_location;
    const QString file = QFileDialog::getOpenFileName(this, windowTitle(), start,
                                                      withAllFiles(m_options.filters).join(QStringLiteral(";;")));
    if (!file.isEmpty()) acceptCandidate(file, false);
}

void AssetPickerDialog::placesMenu(const QPoint& at) {
    QTreeWidgetItem* item = m_places->itemAt(at);
    if (!item || item->data(0, kKindRole).toString() != kFavorite) return;
    const QString path = item->data(0, kPathRole).toString();
    QMenu menu(this);
    menu.addAction(tr("Rename..."), this, [this, path, item]() {
        bool ok = false;
        const QString label =
            QInputDialog::getText(this, tr("Rename Favourite"), tr("Name:"), QLineEdit::Normal, item->text(0), &ok);
        if (ok && renameAssetFavorite(path, label)) buildPlaces();
    });
    menu.addAction(tr("Move Up"), this, [this, path]() {
        if (moveAssetFavorite(path, -1)) buildPlaces();
    });
    menu.addAction(tr("Move Down"), this, [this, path]() {
        if (moveAssetFavorite(path, 1)) buildPlaces();
    });
    menu.addAction(tr("Remove"), this, [this, path]() {
        if (removeAssetFavorite(path)) {
            buildPlaces();
            updateFavoriteButton();
        }
    });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [path]() { QApplication::clipboard()->setText(displayPath(path)); });
    menu.exec(m_places->viewport()->mapToGlobal(at));
}

void AssetPickerDialog::entriesMenu(const QPoint& at) {
    QTreeWidgetItem* item = m_entries->itemAt(at);
    if (!item || item->data(0, kKindRole).toString().isEmpty()) return;
    const QString path = item->data(0, kPathRole).toString();
    QMenu menu(this);
    if (item->data(0, kKindRole).toString() == kFolder) {
        QAction* add = menu.addAction(tr("Add to Favourites"), this, [this, path]() {
            if (addAssetFavorite(path)) {
                buildPlaces();
                updateFavoriteButton();
            }
        });
        add->setEnabled(!path.isEmpty() && !isAssetFavorite(path));
    }
    menu.addAction(tr("Copy Path"), this, [path]() { QApplication::clipboard()->setText(displayPath(path)); });
    menu.exec(m_entries->viewport()->mapToGlobal(at));
}

}  // namespace usdcc::usd
