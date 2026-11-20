#include "usdcc/usd/apply_schema_ui.h"

#include "usdcc/usd/applied_schemas.h"
#include "usdcc/usd/prim_actions.h"
#include "usdcc/usd/relationship_target_dialog.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>

#include <QCoreApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>

#include <algorithm>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
constexpr auto kMaterialBindingApi = "MaterialBindingAPI";
const TfToken kMaterialBinding("material:binding");
constexpr std::size_t kMaxListed = 12;

QString tr(const char* text) { return QCoreApplication::translate("usdcc::usd::askAndApplySchema", text); }
QString qs(const std::string& text) { return QString::fromStdString(text); }

// "    /World/A: why", at most kMaxListed, then how many more.
QString listed(const std::vector<std::pair<SdfPath, std::string>>& prims) {
    QStringList lines;
    for (std::size_t i = 0; i < prims.size() && i < kMaxListed; ++i) {
        const QString why = qs(prims[i].second);
        lines << QStringLiteral("    ") + qs(prims[i].first.GetString()) + (why.isEmpty() ? QString() : QStringLiteral(": ") + why);
    }
    if (prims.size() > kMaxListed) lines << QStringLiteral("    ") + tr("and %1 more").arg(prims.size() - kMaxListed);
    return lines.join(QLatin1Char('\n'));
}

// Why `schema` (single-apply) can't go on `prim`, or empty.
std::string whyNotApply(const UsdPrim& prim, const std::string& schema) {
    if (!prim) return "it isn't there";
    if (!prim_actions::isEditable(prim)) return "it can't be edited here";
    const TfTokenVector applied = prim.GetAppliedSchemas();
    if (std::find(applied.begin(), applied.end(), TfToken(schema)) != applied.end()) return "it has it already";
    std::string why;
    const TfType type = UsdSchemaRegistry::GetAPITypeFromSchemaTypeName(TfToken(schema));
    if (!prim.CanApplyAPI(type, &why)) return why.empty() ? std::string("it can't apply here") : why;
    return {};
}

// Where to apply: one prim's path, or "3 prims".
QString target(const std::vector<SdfPath>& paths) {
    return paths.size() == 1 ? qs(paths.front().GetString()) : tr("%1 prims").arg(paths.size());
}
}  // namespace

bool askAndApplySchema(QWidget* parent, UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths,
                       const QString& schema, bool multipleApply) {
    if (!stage || paths.empty()) return false;
    const UsdStageRefPtr usdStage = stage->usdStage();
    const QString title = tr("Apply %1").arg(schema);

    // Which prims it can go on.
    QString applied = schema;
    std::vector<SdfPath> eligible;
    std::vector<std::pair<SdfPath, std::string>> skipped;
    if (multipleApply) {
        QString instance;
        for (;;) {
            bool ok = false;
            instance = QInputDialog::getText(parent, title, tr("Instance name:"), QLineEdit::Normal, instance, &ok).trimmed();
            if (!ok) return false;
            eligible.clear();
            skipped.clear();
            std::string firstWhy;
            for (const SdfPath& path : paths) {
                const UsdPrim prim = usdStage->GetPrimAtPath(path);
                std::string why;
                if (prim && prim_actions::isEditable(prim) &&
                    canApplyInstance(prim, schema.toStdString(), instance.toStdString(), &why)) {
                    eligible.push_back(path);
                } else {
                    if (why.empty()) why = "it can't be edited here";
                    if (firstWhy.empty()) firstWhy = why;
                    skipped.emplace_back(path, why);
                }
            }
            if (!eligible.empty()) break;
            // The name is the likely problem: ask again, saying why.
            QMessageBox::warning(parent, title, qs(firstWhy));
        }
        applied = schema + QLatin1Char(':') + instance;
    } else {
        for (const SdfPath& path : paths) {
            const std::string why = whyNotApply(usdStage->GetPrimAtPath(path), schema.toStdString());
            if (why.empty()) {
                eligible.push_back(path);
            } else {
                skipped.emplace_back(path, why);
            }
        }
        if (eligible.empty()) {
            QMessageBox::information(parent, title,
                                     paths.size() == 1 ? tr("%1 can't be applied to %2: %3.")
                                                             .arg(schema, qs(paths.front().GetString()), qs(skipped.front().second))
                                                       : tr("%1 can't be applied to any of the %2 prims:\n%3")
                                                             .arg(schema)
                                                             .arg(paths.size())
                                                             .arg(listed(skipped)));
            return false;
        }
    }
    const QString leftOut = skipped.empty() ? QString()
                                            : (skipped.size() == 1 ? tr("1 prim is left out:") : tr("%1 prims are left out:").arg(skipped.size())) +
                                                  QLatin1Char('\n') + listed(skipped);

    // Its material, rather than a binding to find afterwards.
    if (applied == QLatin1String(kMaterialBindingApi)) {
        RelationshipTargetDialog dialog(usdStage, kMaterialBinding.GetString(), SdfPath(), parent);
        dialog.setWindowTitle(eligible.size() == 1
                                  ? tr("Bind a Material to %1").arg(qs(eligible.front().GetName()))
                                  : tr("Bind a Material to %1 prims").arg(eligible.size()));
        dialog.allowNoTarget(tr("Apply Without Material"));
        if (dialog.exec() != QDialog::Accepted) return false;
        if (dialog.selectedPath().IsEmpty()) return prim_actions::applySchema(undo, stage, eligible, applied.toStdString());
        return prim_actions::applySchemaWithTarget(undo, stage, eligible, applied.toStdString(), kMaterialBinding,
                                                   dialog.selectedPath());
    }

    // What it would add: the first prim's, as a sample, with how many prims get any.
    std::vector<TfToken> attributes;
    std::set<TfToken> all;
    for (const SdfPath& path : eligible) {
        const std::vector<TfToken> names = schemaAttributesToAdd(usdStage->GetPrimAtPath(path), applied.toStdString());
        if (attributes.empty()) attributes = names;
        all.insert(names.begin(), names.end());
    }
    // Nothing to add and nothing left out: nothing to ask.
    if (all.empty() && skipped.empty()) return prim_actions::applySchema(undo, stage, eligible, applied.toStdString());

    QMessageBox box(QMessageBox::Question, tr("Apply %1").arg(applied), tr("Apply %1 to %2?").arg(applied, target(eligible)), QMessageBox::Cancel,
                    parent);
    QStringList text;
    if (!all.empty()) {
        QStringList lines;
        std::vector<TfToken> names(all.begin(), all.end());
        if (eligible.size() == 1) names = attributes;
        for (std::size_t i = 0; i < names.size() && i < kMaxListed; ++i) lines << QStringLiteral("    ") + qs(names[i].GetString());
        if (names.size() > kMaxListed) lines << QStringLiteral("    ") + tr("and %1 more").arg(names.size() - kMaxListed);
        const QString question = eligible.size() > 1
                                     ? tr("Add its attributes to the prims, with their default values?")
                                     : (names.size() == 1 ? tr("Add its attribute to the prim, with its default value?")
                                                          : tr("Add its %1 attributes to the prim, with their default values?")
                                                                .arg(names.size()));
        text << question + QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
    }
    if (!leftOut.isEmpty()) text << leftOut;
    box.setInformativeText(text.join(QStringLiteral("\n\n")));
    QPushButton* only = box.addButton(all.empty() ? tr("Apply") : tr("Apply Schema Only"), QMessageBox::AcceptRole);
    QPushButton* add = all.empty() ? nullptr : box.addButton(tr("Apply and Add Attributes"), QMessageBox::AcceptRole);
    box.setDefaultButton(only);
    box.exec();
    if (box.clickedButton() != only && (!add || box.clickedButton() != add)) return false;
    return prim_actions::applySchema(undo, stage, eligible, applied.toStdString(), add && box.clickedButton() == add);
}

}  // namespace usdcc::usd
