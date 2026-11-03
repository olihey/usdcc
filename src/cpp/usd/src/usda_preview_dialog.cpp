#include "usdcc/usd/usda_preview_dialog.h"

#include "usdcc/core/log.h"
#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/prim_context_menu.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>

#include <memory>
#include <utility>
#include <vector>

namespace usdcc::usd {

namespace {
constexpr auto kSizeKey = "UsdaPreview/size";

// One spelling per file, to tell whether a dialog already shows it.
QString fileKey(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.exists() ? info.canonicalFilePath() : path;
#ifdef Q_OS_WIN
    return canonical.toLower();
#else
    return canonical;
#endif
}
}  // namespace

bool canPreviewUsda(const QString& path) {
    const std::string file = path.toStdString();
    return PXR_NS::SdfLayer::Find(file) || PXR_NS::SdfFileFormat::FindByExtension(file);
}

UsdaPreviewDialog* showUsdaPreview(const QString& path, QWidget* parent) {
    if (path.isEmpty() || !canPreviewUsda(path)) return nullptr;
    const QString key = fileKey(path);
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<UsdaPreviewDialog*>(widget);
        if (dialog && dialog->isVisible() && fileKey(dialog->path()) == key) {
            dialog->raise();
            dialog->activateWindow();
            return dialog;
        }
    }
    // Over the main window, even from another preview: closing that one
    // shouldn't take this one with it.
    QWidget* owner = parent ? parent->window() : nullptr;
    while (qobject_cast<UsdaPreviewDialog*>(owner)) {
        owner = owner->parentWidget() ? owner->parentWidget()->window() : nullptr;
    }
    auto* dialog = new UsdaPreviewDialog(path, owner);
    dialog->show();
    return dialog;
}

UsdaPreviewDialog::UsdaPreviewDialog(const QString& path, QWidget* parent) : QDialog(parent), m_path(path) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(path.toStdString());
    const QString name = open ? QString::fromStdString(open->GetDisplayName()) : QFileInfo(path).fileName();
    setWindowTitle(tr("Show USDA - %1").arg(name));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    UsdaView::Host host;
    host.addControls = [this](QBoxLayout* toolbar) {
        usdcc::ui::scaled::margins(toolbar, 4, 4, 4, 4);
        toolbar->addWidget(new usdcc::ui::PanelMenuButton(
            tr("Options"), [this](QMenu* menu) { populateOptionsMenu(menu); }, this));
    };
    // Elsewhere: another preview, with that file's own stage.
    host.openInNew = [this](const UsdaView::Source&, const QString& resolved) {
        return showUsdaPreview(resolved, this) != nullptr;
    };
    m_view = new UsdaView(std::move(host), this);
    layout->addWidget(m_view, 1);

    auto* buttons = new QDialogButtonBox(this);
    usdcc::ui::scaled::margins(buttons->layout(), 8, 6, 8, 8);
    QPushButton* openStage = buttons->addButton(tr("Open as Stage"), QDialogButtonBox::ActionRole);
    QPushButton* closeButton = buttons->addButton(QDialogButtonBox::Close);
    openStage->setAutoDefault(false);
    closeButton->setAutoDefault(false);
    // An in-memory layer has no file for a stage to open.
    openStage->setEnabled(QFileInfo::exists(path));
    connect(openStage, &QPushButton::clicked, this, [this]() {
        if (openStageFile(QFileInfo(m_path).absoluteFilePath())) close();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);
    layout->addWidget(buttons);

    const QSize saved = usdcc::ui::appSettings().value(kSizeKey).toSize();
    resize(saved.isValid() ? saved : usdcc::ui::scaled::size(800, 700));

    // The text at once, the stage behind it.
    UsdaView::Source source;
    source.kind = UsdaView::Source::Kind::File;
    source.identifier = path.toStdString();
    m_view->setStageLoading(true);
    m_view->setSource(source);
    startLoading();
}

void UsdaPreviewDialog::startLoading() {
    // Off this thread as in StageOpener. The result is shared with the thread,
    // which can outlive this dialog: closing it while loading is fine, as USD
    // can't stop an open midway, and the thread just drops what it opened.
    struct Result {
        PXR_NS::UsdStageRefPtr stage;
        std::vector<std::string> errors;
    };
    auto result = std::make_shared<Result>();
    const std::string identifier = m_path.toStdString();
    QThread* thread = QThread::create([result, identifier]() {
        usdcc::core::setThreadName("usda-preview-loader");
        result->stage = StageManager::loadUsdStage(identifier, &result->errors, PXR_NS::UsdStage::LoadNone);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, result]() {
        m_loaded = true;
        if (result->stage) {
            // Not registered anywhere: it's this dialog's alone.
            m_view->adoptStage(std::make_shared<Stage>(result->stage));
        } else {
            m_view->setStageLoading(false);
            m_view->setStatusNote(result->errors.empty()
                                      ? tr("Couldn't load it as a stage")
                                      : tr("Couldn't load it as a stage: %1").arg(QString::fromStdString(result->errors.front())));
        }
    });
    // The app mustn't exit under a running thread.
    connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread]() { thread->wait(); });
    thread->start();
}

void UsdaPreviewDialog::populateOptionsMenu(QMenu* menu) {
    m_view->populateOptionsMenu(menu);
    menu->addSeparator();
    QAction* load = menu->addAction(tr("Load Payloads"), this, [this]() {
        const StageRefPtr& stage = m_view->stage();
        if (!stage) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        stage->usdStage()->Load();
        QApplication::restoreOverrideCursor();
        m_view->refresh();
    });
    const StageRefPtr& stage = m_view->stage();
    load->setEnabled(stage && stage->usdStage()->GetLoadSet().size() < stage->usdStage()->FindLoadable().size());
}

void UsdaPreviewDialog::reject() {
    if (!m_view->closeFind()) QDialog::reject();
}

void UsdaPreviewDialog::done(int result) {
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kSizeKey, size());
    QDialog::done(result);
}

}  // namespace usdcc::usd
