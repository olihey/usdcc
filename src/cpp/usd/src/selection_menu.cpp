#include "usdcc/usd/selection_menu.h"

#include "usdcc/usd/applied_schemas.h"
#include "usdcc/usd/prim_creation_catalog.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/prim_selection.h"
#include "usdcc/usd/select_by_name_dialog.h"
#include "usdcc/usd/stage_manager.h"

#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/prim.h>

#include <QAction>
#include <QTextDocument>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace usdcc::usd {

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

QIcon typeIcon(const std::string& type) { return prim_icons::icon(prim_icons::iconNameForType(PXR_NS::TfToken(type))); }

// The schema's first paragraph, wrapped: a schema's documentation can be long.
QString firstParagraph(const std::string& documentation) {
    const QString text = qs(documentation).trimmed().section(QStringLiteral("\n\n"), 0, 0);
    return text.isEmpty() ? QString() : Qt::convertFromPlainText(text);
}

std::size_t countOf(const std::map<std::string, std::size_t>& counts, const std::string& key) {
    const auto found = counts.find(key);
    return found == counts.end() ? 0 : found->second;
}

}  // namespace

SelectionMenu::SelectionMenu(StageManager* stageManager, QWidget* parent)
    : QMenu(tr("Se&lection"), parent), m_stageManager(stageManager) {
    setObjectName(QStringLiteral("selectionMenu"));
    setToolTipsVisible(true);
    connect(this, &QMenu::aboutToShow, this, &SelectionMenu::rebuild);
}

void SelectionMenu::rebuild() {
    // clear() deletes only the actions; the submenus addMenu() made are this
    // menu's children and would pile up.
    for (QMenu* submenu : findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) submenu->deleteLater();
    clear();

    const StageRefPtr stage = m_stageManager->currentStage();
    StageManager* manager = m_stageManager;
    // Selects what `paths` gives, in the stage the menu was built for.
    auto selecting = [manager, stage](std::function<std::vector<PXR_NS::SdfPath>()> paths) {
        return [manager, stage, paths = std::move(paths)]() { manager->setSelectedPaths(stage, paths()); };
    };
    const PXR_NS::UsdStageRefPtr usdStage = stage ? stage->usdStage() : PXR_NS::UsdStageRefPtr();
    const bool hasSelection = stage && !m_stageManager->selectedPaths(stage).empty();

    QAction* none = addAction(tr("&None"), this, selecting([]() { return std::vector<PXR_NS::SdfPath>(); }));
    none->setObjectName(QStringLiteral("selectNone"));
    none->setEnabled(hasSelection);
    QAction* all = addAction(tr("&All"), this, selecting([usdStage]() { return prim_selection::allPrims(usdStage); }));
    all->setObjectName(QStringLiteral("selectAll"));
    all->setStatusTip(tr("Select every prim of the current stage"));
    all->setEnabled(bool(stage));
    QAction* invert = addAction(tr("&Invert"), this, selecting([manager, stage, usdStage]() {
                                    return prim_selection::inverted(usdStage, manager->selectedPaths(stage));
                                }));
    invert->setObjectName(QStringLiteral("selectInvert"));
    invert->setStatusTip(tr("Select every prim that isn't selected, and none that is"));
    invert->setEnabled(bool(stage));
    addSeparator();

    const prim_selection::Counts counts = prim_selection::count(usdStage);
    // A line with the number of prims that have it, greyed with none.
    auto addEntry = [&](QMenu* into, const QIcon& icon, const QString& label, std::size_t count, const QString& toolTip,
                        std::function<std::vector<PXR_NS::SdfPath>()> paths) {
        // After a tab, QMenu shows it right-aligned, where a shortcut goes.
        const QString text = count ? QStringLiteral("%1\t%2").arg(label).arg(count) : label;
        QAction* action = into->addAction(icon, text, this, selecting(std::move(paths)));
        action->setToolTip(toolTip);
        action->setEnabled(count > 0);
    };

    // By Type: as the Create menu lists them, then any type the stage has that it doesn't.
    QMenu* byType = addMenu(tr("By &Type"));
    byType->setObjectName(QStringLiteral("selectByType"));
    byType->setToolTipsVisible(true);
    std::set<std::string> listedTypes;
    auto addType = [&](QMenu* into, const QString& label, const std::string& type, const QString& toolTip) {
        listedTypes.insert(type);
        addEntry(into, typeIcon(type), label, countOf(counts.types, type), toolTip,
                 [usdStage, type]() { return prim_selection::primsOfType(usdStage, type); });
    };
    addType(byType, tr("Prim (no type)"), std::string(), tr("Prims with no type of their own"));
    bool separated = false;
    for (const PrimCreationCatalog::Category& category : PrimCreationCatalog::instance().categories()) {
        const bool topLevel = category.name == PrimCreationCatalog::kTopLevel;
        if (!topLevel && !separated) {
            byType->addSeparator();
            separated = true;
        }
        QMenu* into = byType;
        if (!topLevel) {
            into = byType->addMenu(typeIcon(category.groups.front().front().type), qs(category.name));
            into->setToolTipsVisible(true);
        }
        for (std::size_t group = 0; group < category.groups.size(); ++group) {
            if (group > 0) into->addSeparator();
            for (const PrimCreationCatalog::Item& item : category.groups[group]) {
                addType(into, qs(item.label), item.type, firstParagraph(item.documentation));
            }
        }
    }
    // A type the Create menu hides, or one USD doesn't know (its plugin isn't loaded).
    QMenu* otherTypes = nullptr;
    for (const auto& [type, count] : counts.types) {
        if (listedTypes.count(type)) continue;
        if (!otherTypes) {
            otherTypes = byType->addMenu(tr("Hidden or Unknown"));
            otherTypes->setToolTipsVisible(true);
        }
        addType(otherTypes, qs(type), type, tr("Not in the Create menu"));
    }

    // By Schema: every applicable API schema by where it comes from, then any the stage
    // has that USD doesn't know.
    QMenu* bySchema = addMenu(tr("By &Schema"));
    bySchema->setObjectName(QStringLiteral("selectBySchema"));
    bySchema->setToolTipsVisible(true);
    std::set<std::string> listedSchemas;
    auto addSchema = [&](QMenu* into, const std::string& schema, const QString& toolTip) {
        listedSchemas.insert(schema);
        addEntry(into, QIcon(), qs(schema), countOf(counts.schemas, schema), toolTip,
                 [usdStage, schema]() { return prim_selection::primsWithSchema(usdStage, schema); });
    };
    for (const SchemaCategory& category : schemaCatalog(PXR_NS::UsdPrim())) {
        // A & in a name is a &, not a mnemonic ("Collections & Color").
        QMenu* into = bySchema->addMenu(qs(category.name).replace(QLatin1Char('&'), QStringLiteral("&&")));
        into->setToolTipsVisible(true);
        for (const SchemaCatalogEntry& schema : category.schemas) {
            addSchema(into, schema.name, firstParagraph(schema.documentation));
        }
    }
    QMenu* unregistered = nullptr;
    for (const auto& [schema, count] : counts.schemas) {
        if (listedSchemas.count(schema)) continue;
        if (!unregistered) {
            bySchema->addSeparator();
            unregistered = bySchema->addMenu(tr("Unregistered"));
            unregistered->setToolTipsVisible(true);
        }
        addSchema(unregistered, schema, tr("Applied, but USD doesn't know it: a plugin not loaded, or a typo"));
    }
    if (!stage) {
        byType->menuAction()->setEnabled(false);
        bySchema->menuAction()->setEnabled(false);
    }

    addSeparator();
    QAction* byName = addAction(tr("By &Name..."));
    byName->setObjectName(QStringLiteral("selectByName"));
    byName->setStatusTip(tr("Select the prims whose name contains some text, or matches a regular expression"));
    byName->setEnabled(bool(stage));
    QWidget* window = parentWidget();
    connect(byName, &QAction::triggered, this, [this, manager, window]() {
        // Queued: the dialog doesn't run inside the menu.
        QMetaObject::invokeMethod(
            this, [manager, window]() { selectByNameInteractively(manager, window); }, Qt::QueuedConnection);
    });
}

}  // namespace usdcc::usd
