#include "usdcc/usd/usda_preview_dialog.h"

#include "usdcc/ui/panel_menu_button.h"
#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/prim_context_menu.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QBoxLayout>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <utility>

namespace usdcc::usd {

namespace {
constexpr auto kSizeKey = "UsdaPreview/size";
}  // namespace

UsdaPreviewDialog* showUsdaPreview(const QString& path, QWidget* parent) {
    if (path.isEmpty() || !canPreviewUsda(path)) return nullptr;
    if (UsdaPreviewDialog* open = raiseOpenPreview<UsdaPreviewDialog>(path)) return open;
    auto* dialog = new UsdaPreviewDialog(path, previewOwner(parent));
    dialog->show();
    return dialog;
}

UsdaPreviewDialog::UsdaPreviewDialog(const QString& path, QWidget* parent) : QDialog(parent), m_path(path) {
    setUpPreviewWindow(this);
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
    loadPreviewStage(m_path, this, [this](const StageRefPtr& stage, const QString& error) {
        m_loaded = true;
        if (stage) {
            m_view->adoptStage(stage);
        } else {
            m_view->setStageLoading(false);
            m_view->setStatusNote(error.isEmpty() ? tr("Couldn't load it as a stage")
                                                  : tr("Couldn't load it as a stage: %1").arg(error));
        }
    });
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
