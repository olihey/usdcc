#include "usdcc/scripting/sessions_dialog.h"

#include "usdcc/scripting/session_controller.h"
#include "usdcc/ui/main_window.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/ui/toast_manager.h"

#include <QBoxLayout>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QUrl>

namespace usdcc::scripting {

namespace {
constexpr auto kSizeKey = "SessionsDialog/size";
enum Role { NameRole = Qt::UserRole, PathRole, ThumbnailRole, ProblemRole };

QString when(const QDateTime& saved) {
    const QDateTime local = saved.toLocalTime();
    const QString time = QLocale().toString(local.time(), QLocale::ShortFormat);
    const QDate today = QDate::currentDate();
    if (local.date() == today) return QObject::tr("today %1").arg(time);
    if (local.date() == today.addDays(-1)) return QObject::tr("yesterday %1").arg(time);
    return QLocale().toString(local.date(), QLocale::ShortFormat);
}

QIcon placeholderIcon(const QSize& size) {
    QPixmap pixmap(size);
    pixmap.fill(QColor(0x30, 0x30, 0x30));
    return QIcon(pixmap);
}
}  // namespace

SessionsDialog::SessionsDialog(SessionController* sessions, const QImage& preview, QWidget* parent)
    : QDialog(parent), m_sessions(sessions), m_preview(preview) {
    setWindowTitle(tr("Sessions"));
    setObjectName(QStringLiteral("sessionsDialog"));
    setAcceptDrops(true);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search…"));
    m_search->setClearButtonEnabled(true);
    m_search->setObjectName(QStringLiteral("sessionsSearch"));

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("sessionsList"));
    m_list->setIconSize(usdcc::ui::scaled::size(96, 54));
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    m_image = new QLabel(this);
    m_image->setObjectName(QStringLiteral("sessionPreview"));
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(usdcc::ui::scaled::size(320, 180));
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_details = new QLabel(this);
    m_details->setObjectName(QStringLiteral("sessionDetails"));
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(m_image, 3);
    rightLayout->addWidget(m_details, 2);

    auto* split = new QSplitter(this);
    split->addWidget(m_list);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    m_name = new QLineEdit(m_sessions->currentName(), this);
    m_name->setObjectName(QStringLiteral("sessionName"));
    m_name->setPlaceholderText(tr("Name the session to save"));
    auto* save = new QPushButton(tr("Save"), this);
    save->setObjectName(QStringLiteral("saveSession"));
    auto* saveRow = new QHBoxLayout;
    saveRow->addWidget(new QLabel(tr("Name:"), this));
    saveRow->addWidget(m_name, 1);
    saveRow->addWidget(save);

    m_rename = new QPushButton(tr("Rename..."), this);
    m_delete = new QPushButton(tr("Delete"), this);
    auto* importButton = new QPushButton(tr("Import..."), this);
    m_export = new QPushButton(tr("Export..."), this);
    auto* folder = new QPushButton(tr("Open Folder"), this);
    m_load = new QPushButton(tr("Load"), this);
    m_load->setObjectName(QStringLiteral("loadSession"));
    auto* close = new QPushButton(tr("Close"), this);
    auto* buttons = new QHBoxLayout;
    for (QPushButton* button : {m_rename, m_delete, importButton, m_export, folder}) buttons->addWidget(button);
    buttons->addStretch(1);
    buttons->addWidget(m_load);
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_search);
    layout->addWidget(split, 1);
    layout->addLayout(saveRow);
    layout->addLayout(buttons);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* item = m_list->item(i);
            item->setHidden(!item->data(NameRole).toString().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this]() { updateDetails(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() { loadSelected(); });
    connect(save, &QPushButton::clicked, this, [this]() { saveAs(m_name->text()); });
    connect(m_name, &QLineEdit::returnPressed, this, [this]() { saveAs(m_name->text()); });
    connect(m_load, &QPushButton::clicked, this, [this]() { loadSelected(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_rename, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString to = QInputDialog::getText(this, tr("Rename Session"), tr("New name:"), QLineEdit::Normal, selected(), &ok);
        if (ok && !to.trimmed().isEmpty()) renameSelected(to);
    });
    connect(m_delete, &QPushButton::clicked, this, [this]() {
        if (selected().isEmpty()) return;
        if (QMessageBox::question(this, tr("Delete Session"),
                                  tr("Move the session \"%1\" to the recycle bin?").arg(selected())) == QMessageBox::Yes) {
            deleteSelected();
        }
    });
    connect(importButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Session"), QString(), tr("Sessions (*.json)"));
        if (!path.isEmpty()) importFile(path);
    });
    connect(m_export, &QPushButton::clicked, this, [this]() {
        if (selected().isEmpty()) return;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Session"), QDir::home().filePath(usdcc::ui::SessionStore::fileNameFor(selected())),
            tr("Sessions (*.json)"));
        if (!path.isEmpty()) exportSelected(path);
    });
    connect(folder, &QPushButton::clicked, this, [this]() {
        QDir().mkpath(m_sessions->store().folder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_sessions->store().folder()));
    });

    m_thumbnails = new QTimer(this);
    m_thumbnails->setInterval(0);
    connect(m_thumbnails, &QTimer::timeout, this, &SessionsDialog::loadNextThumbnail);

    const QSize saved = usdcc::ui::appSettings().value(kSizeKey).toSize();
    resize(saved.isValid() ? saved : usdcc::ui::scaled::size(960, 600));
    refresh();
}

SessionsDialog::~SessionsDialog() {
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kSizeKey, size());
}

void SessionsDialog::showDialog(SessionController* sessions, QWidget* parent) {
    // Before the dialog is on screen, so it isn't in the picture.
    const QImage preview = sessions->takePreview();
    SessionsDialog dialog(sessions, preview, parent);
    dialog.exec();
}

QString SessionsDialog::selected() const {
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(NameRole).toString() : QString();
}

void SessionsDialog::select(const QString& name) {
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(NameRole).toString().compare(name, Qt::CaseInsensitive) == 0) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

void SessionsDialog::refresh() {
    const QString keep = selected().isEmpty() ? m_sessions->currentName() : selected();
    m_list->clear();
    QList<usdcc::ui::SessionInfo> sessions = m_sessions->store().list();
    // The Last session first (§4.5).
    std::stable_partition(sessions.begin(), sessions.end(), [](const usdcc::ui::SessionInfo& info) {
        return info.name == QLatin1String(usdcc::ui::session::kLastSessionName);
    });
    const QIcon placeholder = placeholderIcon(m_list->iconSize());
    for (const usdcc::ui::SessionInfo& info : std::as_const(sessions)) {
        auto* item = new QListWidgetItem(placeholder, info.name + QLatin1Char('\n') + when(info.saved), m_list);
        item->setData(NameRole, info.name);
        item->setData(PathRole, info.path);
        item->setData(ProblemRole, info.problem);
        item->setToolTip(QDir::toNativeSeparators(info.path));
        if (!m_search->text().isEmpty()) item->setHidden(!info.name.contains(m_search->text(), Qt::CaseInsensitive));
    }
    select(keep);
    if (!m_list->currentItem() && m_list->count() > 0) m_list->setCurrentRow(0);
    updateDetails();
    m_thumbnails->start();
}

void SessionsDialog::loadNextThumbnail() {
    // One a tick, so a long list shows at once and fills in.
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        if (item->data(ThumbnailRole).toBool()) continue;
        item->setData(ThumbnailRole, true);
        const auto session = m_sessions->store().read(item->data(PathRole).toString());
        const QImage preview = session ? usdcc::ui::session::preview(*session) : QImage();
        if (!preview.isNull()) {
            const QSize size = m_list->iconSize() * devicePixelRatioF();
            QPixmap pixmap = QPixmap::fromImage(preview.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            pixmap.setDevicePixelRatio(devicePixelRatioF());
            item->setIcon(QIcon(pixmap));
        }
        return;
    }
    m_thumbnails->stop();
}

void SessionsDialog::showPreview() {
    if (m_shown.isNull()) {
        m_image->setText(m_list->currentItem() ? tr("No preview") : QString());
        return;
    }
    const QSize box = m_image->contentsRect().size() * devicePixelRatioF();
    QPixmap pixmap = QPixmap::fromImage(m_shown.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    m_image->setPixmap(pixmap);
}

void SessionsDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    showPreview();
}

void SessionsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Once it's laid out at its size.
    QTimer::singleShot(0, this, &SessionsDialog::showPreview);
}

void SessionsDialog::updateDetails() {
    const QListWidgetItem* item = m_list->currentItem();
    m_load->setEnabled(item && item->data(ProblemRole).toString().isEmpty());
    m_rename->setEnabled(item);
    m_delete->setEnabled(item && item->data(NameRole).toString() != QLatin1String(usdcc::ui::session::kLastSessionName));
    m_export->setEnabled(item);
    if (!item) {
        m_shown = QImage();
        m_image->clear();
        m_details->setText(m_list->count() == 0 ? tr("No sessions yet. Name one below and Save.") : QString());
        return;
    }
    QString error;
    const auto session = m_sessions->store().read(item->data(PathRole).toString(), &error);
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(item->data(NameRole).toString().toHtmlEscaped());
    if (!session) {
        m_shown = QImage();
        m_image->clear();
        lines << QStringLiteral("⚠ %1").arg(error.toHtmlEscaped());
        m_details->setText(lines.join(QStringLiteral("<br>")));
        return;
    }
    m_shown = usdcc::ui::session::preview(*session);
    showPreview();
    const QDateTime saved = QDateTime::fromString(session->value(QStringLiteral("saved")).toString(), Qt::ISODateWithMs);
    if (saved.isValid()) lines << tr("Saved %1").arg(when(saved));

    QStringList stages;
    QStringList warnings;
    for (const QJsonValue& value : session->value(QStringLiteral("stages")).toArray()) {
        const QString file = value.toObject().value(QStringLiteral("file")).toString();
        if (file.isEmpty()) {
            stages << tr("one in memory");
            continue;
        }
        stages << QFileInfo(file).fileName().toHtmlEscaped();
        if (!QFileInfo::exists(file)) {
            warnings << tr("%1 isn't there: %2").arg(QFileInfo(file).fileName(), QDir::toNativeSeparators(file)).toHtmlEscaped();
        }
    }
    lines << tr("Stages: %1").arg(stages.isEmpty() ? tr("none") : stages.join(QStringLiteral(", ")));
    QStringList plugins;
    for (const QJsonValue& value : session->value(QStringLiteral("plugins")).toArray()) {
        const QJsonObject plugin = value.toObject();
        plugins << plugin.value(QStringLiteral("name")).toString(plugin.value(QStringLiteral("id")).toString()).toHtmlEscaped();
    }
    lines << tr("Plugins: %1").arg(plugins.isEmpty() ? tr("none") : plugins.join(QStringLiteral(", ")));
    const SessionController::PluginDifference difference = m_sessions->pluginDifference(*session);
    for (const QString& missing : difference.missing) warnings << tr("The plugin %1 isn't here").arg(missing).toHtmlEscaped();
    if (difference.needsRestart()) warnings << tr("Loading it restarts the app with other plugins").toHtmlEscaped();
    for (const QString& warning : std::as_const(warnings)) lines << QStringLiteral("⚠ %1").arg(warning);
    m_details->setText(lines.join(QStringLiteral("<br>")));
}

bool SessionsDialog::loadSelected() {
    const QString name = selected();
    if (name.isEmpty()) return false;
    SessionController::LoadOptions options;
    options.source = name;
    QString error;
    const SessionController::LoadResult result = m_sessions->load(name, options, &error);
    if (result == SessionController::LoadResult::Failed) {
        QMessageBox::warning(this, tr("Load Session"), error);
        return false;
    }
    if (result == SessionController::LoadResult::Cancelled) return false;
    accept();
    if (result == SessionController::LoadResult::Loaded && !m_sessions->warnings().isEmpty()) {
        if (auto* window = qobject_cast<usdcc::ui::MainWindow*>(parentWidget())) {
            usdcc::core::ToastRequest toast;
            toast.kind = usdcc::core::ToastKind::Warning;
            toast.title = tr("Loaded \"%1\", not all of it").arg(name).toStdString();
            toast.body = m_sessions->warnings().join(QLatin1Char('\n')).toStdString();
            window->toasts()->show(toast);
        }
    }
    return true;
}

bool SessionsDialog::saveAs(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        m_name->setFocus();
        return false;
    }
    if (m_sessions->store().contains(trimmed) && trimmed.compare(m_sessions->currentName(), Qt::CaseInsensitive) != 0 &&
        QMessageBox::question(this, tr("Save Session"), tr("Replace the session \"%1\"?").arg(trimmed)) != QMessageBox::Yes) {
        return false;
    }
    QString error;
    if (!m_sessions->save(trimmed, m_preview, true, &error)) {
        if (!error.isEmpty()) QMessageBox::warning(this, tr("Save Session"), error);
        return false;
    }
    refresh();
    select(trimmed);
    return true;
}

bool SessionsDialog::renameSelected(const QString& to) {
    const QString from = selected();
    QString error;
    if (from.isEmpty() || !m_sessions->store().rename(from, to, &error)) {
        if (!error.isEmpty()) QMessageBox::warning(this, tr("Rename Session"), error);
        return false;
    }
    refresh();
    select(to.trimmed());
    return true;
}

bool SessionsDialog::deleteSelected() {
    QString error;
    if (!m_sessions->store().remove(selected(), &error)) {
        QMessageBox::warning(this, tr("Delete Session"), error);
        return false;
    }
    refresh();
    return true;
}

bool SessionsDialog::importFile(const QString& path) {
    const QString name = usdcc::ui::SessionStore::nameInFile(path);
    bool replace = false;
    if (!name.isEmpty() && m_sessions->store().contains(name)) {
        if (QMessageBox::question(this, tr("Import Session"), tr("Replace the session \"%1\"?").arg(name)) != QMessageBox::Yes) {
            return false;
        }
        replace = true;
    }
    QString error;
    const QString imported = m_sessions->store().importFile(path, replace, &error);
    if (imported.isEmpty()) {
        QMessageBox::warning(this, tr("Import Session"), error);
        return false;
    }
    refresh();
    select(imported);
    return true;
}

bool SessionsDialog::exportSelected(const QString& path) {
    QString error;
    if (!m_sessions->store().exportTo(selected(), path, &error)) {
        QMessageBox::warning(this, tr("Export Session"), error);
        return false;
    }
    return true;
}

void SessionsDialog::dragEnterEvent(QDragEnterEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void SessionsDialog::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            importFile(url.toLocalFile());
        }
    }
    event->acceptProposedAction();
}

}  // namespace usdcc::scripting
