#include "usdcc/usd/attributes_view_panel.h"

#include "usdcc/ui/scaled_metrics.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/property_sheet.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include "usdcc/usd/variant_editor_dialog.h"

#include <pxr/usd/usd/prim.h>

#include <QAction>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {
constexpr auto kAuthoredOnlyKey = "AttributeEditor/authoredOnly";
constexpr auto kShowHiddenKey = "AttributeEditor/showHidden";
}  // namespace

AttributesViewPanel::AttributesViewPanel(StageManager* stageManager, StageRefPtr stage,
                                          std::shared_ptr<UndoManager> undoManager, const QString& title,
                                          QWidget* parent)
    : usdcc::ui::ViewPanel(stageManager, std::move(stage), title, parent), m_undoManager(std::move(undoManager)) {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QWidget(container);
    toolbar->setObjectName(QStringLiteral("panelToolbar"));  // styled by theme/dark.qss
    toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* toolbarLayout = new QHBoxLayout(toolbar);

    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(tr("Search attributes"));
    m_search->setClearButtonEnabled(true);

    m_authoredOnly = new QAction(tr("Authored Only"), this);
    m_authoredOnly->setCheckable(true);
    m_showHidden = new QAction(tr("Hidden Properties"), this);
    m_showHidden->setCheckable(true);
    {
        QSettings settings = usdcc::ui::appSettings();
        m_authoredOnly->setChecked(settings.value(kAuthoredOnlyKey, false).toBool());
        m_showHidden->setChecked(settings.value(kShowHiddenKey, false).toBool());
    }

    addPanelControls(toolbarLayout);
    toolbarLayout->addWidget(m_search, 1);
    // One right edge for the whole panel: the scroll bar's width (theme/dark.qss),
    // whose room is always kept, so that nothing moves when it comes and goes.
    usdcc::ui::scaled::margins(toolbarLayout, 4, 4, 10, 4);

    m_variantBanner = new VariantEditBanner(container);
    m_sheet = new PropertySheet(stageManager, m_undoManager, PropertySheet::Mode::Full, container);
    m_sheet->setAuthoredOnly(m_authoredOnly->isChecked());
    m_sheet->setShowHidden(m_showHidden->isChecked());

    layout->addWidget(toolbar);
    layout->addWidget(m_variantBanner);
    layout->addWidget(m_sheet, 1);
    setContentWidget(container);

    connect(m_search, &QLineEdit::textChanged, m_sheet, &PropertySheet::setSearchText);
    connect(m_sheet, &PropertySheet::rebuilt, this, &AttributesViewPanel::updateVariantDetail);
    for (QAction* action : {m_authoredOnly, m_showHidden}) {
        connect(action, &QAction::toggled, this, [this]() {
            QSettings settings = usdcc::ui::appSettings();
            settings.setValue(kAuthoredOnlyKey, m_authoredOnly->isChecked());
            settings.setValue(kShowHiddenKey, m_showHidden->isChecked());
            m_sheet->setAuthoredOnly(m_authoredOnly->isChecked());
            m_sheet->setShowHidden(m_showHidden->isChecked());
        });
    }

    m_selectionChangedConnection = stageManager->selectionChanged.connect(
        [this](StageRefPtr changedStage, std::vector<PXR_NS::SdfPath> paths) {
            onStageSelectionChanged(changedStage, std::move(paths));
        });

    refreshStageCombo();
    // See ViewportViewPanel's constructor for why this is explicit.
    onStageChanged(currentStage());
}

usdcc::ui::ViewPanel* AttributesViewPanel::duplicate(QWidget* parent) const {
    return new AttributesViewPanel(stageManager(), currentStage(), m_undoManager, windowTitle(), parent);
}

void AttributesViewPanel::populateOptionsMenu(QMenu* menu) {
    menu->addAction(m_authoredOnly);
    menu->addAction(m_showHidden);
}

void AttributesViewPanel::populateSchemaMenu(QMenu* menu) { m_sheet->populateSchemaMenu(menu); }

void AttributesViewPanel::onStageChanged(const StageRefPtr& stage) {
    m_variantBanner->setStage(stage);
    const std::vector<SdfPath> paths = stageManager()->selectedPaths(stage);
    // The path first, so the new stage's rows are built once.
    m_sheet->blockSignals(true);
    m_sheet->setPrimPath(paths.empty() ? SdfPath() : paths.front());
    m_sheet->blockSignals(false);
    m_sheet->setStage(stage);
}

void AttributesViewPanel::onStageSelectionChanged(StageRefPtr stage, std::vector<PXR_NS::SdfPath> paths) {
    if (stage != currentStage()) {
        return;
    }
    m_sheet->setPrimPath(paths.empty() ? SdfPath() : paths.front());
}

void AttributesViewPanel::updateVariantDetail() {
    const UsdPrim prim = m_sheet->currentPrim();
    m_variantBanner->setDetail(prim && currentStage() && currentStage()->variantEdit() && !prim_actions::isEditable(prim)
                                   ? tr("%1 is outside it, so it can't be edited here.")
                                         .arg(QString::fromStdString(prim.GetPath().GetString()))
                                   : QString());
}

}  // namespace usdcc::usd
