#include "usdcc/usd/create_menu.h"

#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/token.h>

#include <QAction>
#include <QCoreApplication>
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

QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::PrimCreateMenu", text); }

}  // namespace

void addCreateEntries(QMenu* menu, StageManager* stageManager, const StageRefPtr& stage, UndoManager* undo,
                      const prim_actions::CreateTarget& target) {
    const bool usable = target.problem.isEmpty();
    QString where = target.problem;
    if (usable) {
        where = target.parent.IsAbsoluteRootPath() ? tr("At the root of the stage") : tr("In %1").arg(qs(target.parent.GetString()));
        if (const std::optional<VariantEdit>& edit = stage->variantEdit()) {
            where = tr("%1, in %2=%3").arg(where, qs(edit->set), qs(edit->variant));
        }
    }
    if (stage) {  // with no stage there's no "where" to say
        menu->addAction(where)->setEnabled(false);
        menu->addSeparator();
    }

    const PXR_NS::SdfPath parent = target.parent;
    auto addEntry = [=](QMenu* into, const QString& label, const std::string& type, const QString& toolTip) {
        QAction* action = into->addAction(typeIcon(type), label, menu, [=]() {
            const PXR_NS::SdfPath path = prim_actions::createPrim(*undo, stage, parent, type);
            if (!path.IsEmpty() && stageManager) stageManager->setSelectedPaths(stage, {path});
        });
        action->setToolTip(toolTip);
        action->setEnabled(usable);
    };
    addEntry(menu, tr("Prim (no type)"), std::string(),
             tr("A prim with no type of its own, which takes the type of what it references or has as a payload."));

    bool separated = false;
    for (const PrimCreationCatalog::Category& category : PrimCreationCatalog::instance().categories()) {
        const bool topLevel = category.name == PrimCreationCatalog::kTopLevel;
        if (!topLevel && !separated) {
            menu->addSeparator();
            separated = true;
        }
        QMenu* into = menu;
        if (!topLevel) {
            into = menu->addMenu(typeIcon(category.groups.front().front().type), qs(category.name));
            into->setToolTipsVisible(true);
        }
        for (std::size_t group = 0; group < category.groups.size(); ++group) {
            if (group > 0) into->addSeparator();
            for (const PrimCreationCatalog::Item& item : category.groups[group]) {
                addEntry(into, qs(item.label), item.type, firstParagraph(item.documentation));
            }
        }
    }
}

PrimCreateMenu::PrimCreateMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, QWidget* parent)
    : QMenu(QCoreApplication::translate("usdcc::usd::PrimCreateMenu", "&Create"), parent),
      m_stageManager(stageManager),
      m_undo(std::move(undo)) {
    setToolTipsVisible(true);
    connect(this, &QMenu::aboutToShow, this, &PrimCreateMenu::rebuild);
}

void PrimCreateMenu::rebuild() {
    // clear() deletes only the actions; the submenus addMenu() made are this
    // menu's children and would pile up.
    for (QMenu* submenu : findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) submenu->deleteLater();
    clear();
    const StageRefPtr stage = m_stageManager->currentStage();
    prim_actions::CreateTarget target;
    if (stage) {
        target = prim_actions::createTarget(stage, m_stageManager->selectedPaths(stage));
    } else {
        target.problem = tr("No stage is open");  // what can be made is still listed, disabled
    }
    addCreateEntries(this, m_stageManager, stage, m_undo.get(), target);
}

}  // namespace usdcc::usd
