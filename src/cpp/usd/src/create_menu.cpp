#include "usdcc/usd/create_menu.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/token.h>

#include <QAction>
#include <QTextDocument>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

QIcon typeIcon(const std::string& type) { return prim_icons::icon(prim_icons::iconNameForType(PXR_NS::TfToken(type))); }

// The schema's first paragraph, wrapped: a schema's documentation can be long.
QString firstParagraph(const std::string& documentation) {
    return Qt::convertFromPlainText(qs(documentation).trimmed().section(QStringLiteral("\n\n"), 0, 0));
}

}  // namespace

PrimCreateMenu::PrimCreateMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, QWidget* parent)
    : QMenu(tr("&Create"), parent), m_stageManager(stageManager), m_undo(std::move(undo)) {
    setToolTipsVisible(true);
    connect(this, &QMenu::aboutToShow, this, &PrimCreateMenu::rebuild);
    m_currentStageChanged = m_stageManager->currentStageChanged.connect([this](const StageRefPtr&) { updateEnabled(); });
    updateEnabled();
}

void PrimCreateMenu::updateEnabled() { menuAction()->setEnabled(static_cast<bool>(m_stageManager->currentStage())); }

void PrimCreateMenu::rebuild() {
    // clear() deletes only the actions; the submenus addMenu() made are this
    // menu's children and would pile up.
    for (QMenu* submenu : findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) submenu->deleteLater();
    clear();
    const StageRefPtr stage = m_stageManager->currentStage();
    if (!stage) return;

    const prim_actions::CreateTarget target = prim_actions::createTarget(stage, m_stageManager->selectedPaths(stage));
    const bool usable = target.problem.isEmpty();
    QString where = target.problem;
    if (usable) {
        where = target.parent.IsAbsoluteRootPath() ? tr("At the root of the stage") : tr("In %1").arg(qs(target.parent.GetString()));
        if (const std::optional<VariantEdit>& edit = stage->variantEdit()) {
            where = tr("%1, in %2=%3").arg(where, qs(edit->set), qs(edit->variant));
        }
    }
    addAction(where)->setEnabled(false);
    addSeparator();

    auto addEntry = [this, usable](QMenu* menu, const QString& label, const std::string& type, const QString& toolTip) {
        QAction* action = menu->addAction(typeIcon(type), label, this, [this, type]() { create(type); });
        action->setToolTip(toolTip);
        action->setEnabled(usable);
    };
    addEntry(this, tr("Prim (no type)"), std::string(),
             tr("A prim with no type of its own, which takes the type of what it references or has as a payload."));

    bool separated = false;
    for (const PrimCreationCatalog::Category& category : PrimCreationCatalog::instance().categories()) {
        const bool topLevel = category.name == PrimCreationCatalog::kTopLevel;
        if (!topLevel && !separated) {
            addSeparator();
            separated = true;
        }
        QMenu* menu = this;
        if (!topLevel) {
            menu = addMenu(typeIcon(category.groups.front().front().type), qs(category.name));
            menu->setToolTipsVisible(true);
        }
        for (std::size_t group = 0; group < category.groups.size(); ++group) {
            if (group > 0) menu->addSeparator();
            for (const PrimCreationCatalog::Item& item : category.groups[group]) {
                addEntry(menu, qs(item.label), item.type, firstParagraph(item.documentation));
            }
        }
    }
}

void PrimCreateMenu::create(const std::string& type) {
    const StageRefPtr stage = m_stageManager->currentStage();
    if (!stage) return;
    // Afresh: the selection could have changed since the menu opened, through a script.
    const prim_actions::CreateTarget target = prim_actions::createTarget(stage, m_stageManager->selectedPaths(stage));
    if (!target.problem.isEmpty()) return;
    const PXR_NS::SdfPath path = prim_actions::createPrim(*m_undo, stage, target.parent, type);
    if (!path.IsEmpty()) m_stageManager->setSelectedPaths(stage, {path});
}

}  // namespace usdcc::usd
