#include "usdcc/usd/prim_tree_dialog.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/prim_tree_filter.h"
#include "usdcc/usd/prim_tree_widget.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace usdcc::usd {

namespace {
constexpr auto kSizeKey = "PrimTreeDialog/size";
}  // namespace

PrimTreeDialog* showPrimTree(const QString& path, QWidget* parent) {
    if (path.isEmpty() || !canPreviewUsda(path)) return nullptr;
    if (PrimTreeDialog* open = raiseOpenPreview<PrimTreeDialog>(path)) return open;
    auto* dialog = new PrimTreeDialog(path, previewOwner(parent));
    dialog->show();
    return dialog;
}

PrimTreeDialog::PrimTreeDialog(const QString& path, QWidget* parent) : QDialog(parent), m_path(path) {
    setUpPreviewWindow(this);
    const PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(path.toStdString());
    const QString name = open ? QString::fromStdString(open->GetDisplayName()) : QFileInfo(path).fileName();
    setWindowTitle(tr("Show Prims - %1").arg(name));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tree = new PrimTreeWidget(this);
    m_tree->filterBar()->setEnabled(false);  // until there's something to filter
    connect(m_tree, &PrimTreeWidget::filterApplied, this, &PrimTreeDialog::updateStatus);
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(tr("Loading stage..."), this);
    m_status->setObjectName(QStringLiteral("propertyNote"));
    m_status->setContentsMargins(8, 2, 8, 2);
    layout->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(this);
    usdcc::ui::scaled::margins(buttons->layout(), 8, 6, 8, 8);
    m_loadPayloads = buttons->addButton(tr("Load Payloads"), QDialogButtonBox::ActionRole);
    QPushButton* closeButton = buttons->addButton(QDialogButtonBox::Close);
    m_loadPayloads->setAutoDefault(false);
    closeButton->setAutoDefault(false);
    m_loadPayloads->setEnabled(false);
    connect(m_loadPayloads, &QPushButton::clicked, this, [this]() {
        if (!m_stage) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_stage->usdStage()->Load();  // its change notice rebuilds the tree
        QApplication::restoreOverrideCursor();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);
    layout->addWidget(buttons);

    const QSize saved = usdcc::ui::appSettings().value(kSizeKey).toSize();
    resize(saved.isValid() ? saved : usdcc::ui::scaled::size(480, 640));

    loadPreviewStage(path, this,
                     [this](const StageRefPtr& stage, const QString& error) { onStageLoaded(stage, error); });
}

QTreeView* PrimTreeDialog::treeView() const { return m_tree->treeView(); }

void PrimTreeDialog::onStageLoaded(const StageRefPtr& stage, const QString& error) {
    m_loaded = true;
    if (!stage) {
        m_status->setText(error.isEmpty() ? tr("Couldn't load it as a stage")
                                          : tr("Couldn't load it as a stage: %1").arg(error));
        m_status->setWordWrap(true);
        return;
    }
    m_stage = stage;
    m_tree->setStage(stage->usdStage());
    m_tree->filterBar()->setEnabled(true);
    // Layers are shared with the app's stages, so its edits can recompose this
    // one; only the ones that touch the hierarchy rebuild the tree.
    m_stageChangedConnection = stage->changed.connect([this](const StageChanges& changes) {
        const bool hierarchy = std::any_of(changes.resyncedPaths.begin(), changes.resyncedPaths.end(),
                                           [](const PXR_NS::SdfPath& p) { return p.IsPrimPath() || p.IsAbsoluteRootPath(); });
        if (!hierarchy || m_refreshQueued) return;
        m_refreshQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_refreshQueued = false;
            m_tree->refresh();
            updateStatus();
        });
    });
    updateStatus();
}

void PrimTreeDialog::setFilter(const QString& text) { m_tree->setFilter(text); }

void PrimTreeDialog::updateStatus() {
    if (!m_stage) return;
    const PXR_NS::UsdStageRefPtr& stage = m_stage->usdStage();
    int prims = 0;
    for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange::Stage(stage, PXR_NS::UsdPrimAllPrimsPredicate)) {
        (void)prim;
        ++prims;
    }
    const QLocale locale;
    const QString dot = QStringLiteral("  \xC2\xB7  ");
    QString status = prims == 1 ? tr("1 prim") : tr("%1 prims").arg(locale.toString(prims));
    const std::size_t loadable = stage->FindLoadable().size();
    const std::size_t loaded = stage->GetLoadSet().size();
    if (loaded < loadable) {
        const int missing = int(loadable - loaded);
        status += dot + (missing == 1 ? tr("1 payload not loaded") : tr("%1 payloads not loaded").arg(locale.toString(missing)));
    }
    m_loadPayloads->setEnabled(loaded < loadable);
    if (m_tree->isFiltering()) {
        const int matches = m_tree->matchCount();
        status = (matches == 1 ? tr("1 match") : tr("%1 matches").arg(locale.toString(matches))) + dot + status;
        if (!stage->GetPrototypes().empty()) status += dot + tr("instances: expand to search inside");
    }
    m_status->setText(status);
}

void PrimTreeDialog::reject() {
    if (!m_tree->isFiltering()) {
        QDialog::reject();
    } else {
        m_tree->clearFilter();
    }
}

void PrimTreeDialog::done(int result) {
    QSettings settings = usdcc::ui::appSettings();
    settings.setValue(kSizeKey, size());
    QDialog::done(result);
}

}  // namespace usdcc::usd
