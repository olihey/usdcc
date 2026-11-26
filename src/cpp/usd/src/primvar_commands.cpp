#include "usdcc/usd/primvar_commands.h"

#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSpinBox>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

UsdGeomPrimvar firstPrimvar(const PrimvarCommandContext& context) {
    if (!context.stage || context.primvar.empty()) return {};
    for (const SdfPath& path : context.primsWith()) {
        const UsdPrim prim = context.stage->usdStage()->GetPrimAtPath(path);
        if (const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken(context.primvar))) return primvar;
    }
    return {};
}

std::optional<QString> askText(const PrimvarCommandContext& context, const QString& title, const QString& label, const QString& text) {
    if (context.askText) return context.askText(title, label, text);
    bool ok = false;
    const QString answer = QInputDialog::getText(context.dialogParent, title, label, QLineEdit::Normal, text, &ok);
    if (!ok) return std::nullopt;
    return answer.trimmed();
}

std::optional<QString> askItem(const PrimvarCommandContext& context, const QString& title, const QString& label, const QStringList& items) {
    if (context.askItem) return context.askItem(title, label, items);
    bool ok = false;
    const QString answer = QInputDialog::getItem(context.dialogParent, title, label, items, 0, false, &ok);
    if (!ok) return std::nullopt;
    return answer;
}

std::optional<int> askNumber(const PrimvarCommandContext& context, const QString& title, const QString& label, int value, int low, int high) {
    if (context.askNumber) return context.askNumber(title, label, value, low, high);
    bool ok = false;
    const int answer = QInputDialog::getInt(context.dialogParent, title, label, value, low, high, 1, &ok);
    if (!ok) return std::nullopt;
    return answer;
}

// Create Primvar's dialog: a name, a type, an interpolation and an element size.
std::optional<PrimvarCreateRequest> askCreate(const PrimvarCommandContext& context, const QStringList& prims) {
    if (context.askCreate) return context.askCreate(prims);
    QDialog dialog(context.dialogParent);
    dialog.setWindowTitle(QObject::tr("Create Primvar"));
    auto* form = new QFormLayout(&dialog);
    auto* on = new QLabel(prims.size() == 1 ? prims.front() : QObject::tr("%1 prims").arg(prims.size()), &dialog);
    form->addRow(QObject::tr("On:"), on);
    auto* name = new QLineEdit(&dialog);
    name->setPlaceholderText(QStringLiteral("displayColor, st1, weight"));
    form->addRow(QObject::tr("Name:"), name);
    auto* type = new QComboBox(&dialog);
    for (const SdfValueTypeName& each : primvarCreateTypes()) type->addItem(qs(each.GetAsToken().GetString()));
    form->addRow(QObject::tr("Type:"), type);
    auto* interpolation = new QComboBox(&dialog);
    for (const TfToken& each : {UsdGeomTokens->constant, UsdGeomTokens->uniform, UsdGeomTokens->varying, UsdGeomTokens->vertex,
                                UsdGeomTokens->faceVarying}) {
        interpolation->addItem(qs(each.GetString()));
    }
    interpolation->setCurrentText(QStringLiteral("vertex"));
    form->addRow(QObject::tr("Interpolation:"), interpolation);
    auto* size = new QSpinBox(&dialog);
    size->setRange(1, 64);
    form->addRow(QObject::tr("Element size:"), size);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    PrimvarCreateRequest request;
    request.name = name->text().trimmed().toStdString();
    request.type = SdfSchema::GetInstance().FindType(type->currentText().toStdString());
    request.interpolation = TfToken(interpolation->currentText().toStdString());
    request.elementSize = size->value();
    return request;
}

// A command for each prim that has the menu's primvar.
PrimvarCommand onEach(const char* id, const QString& label, const char* step,
                      std::function<std::optional<std::function<PrimvarEdit(const UsdPrim&)>>(const PrimvarCommandContext&)> prepare) {
    PrimvarCommand command;
    command.id = id;
    command.label = label;
    command.run = [step, prepare = std::move(prepare)](const PrimvarCommandContext& context) {
        const std::optional<std::function<PrimvarEdit(const UsdPrim&)>> edit = prepare(context);
        if (!edit) return;
        runPrimvarEdit(context, QObject::tr(step).arg(qs(context.primvar)), context.primsWith(), *edit);
    };
    command.applies = [](const PrimvarCommandContext& context) { return !context.primvar.empty(); };
    return command;
}

void addBuiltIns(std::vector<PrimvarCommand>& commands) {
    commands.push_back(onEach("usdcc.rename", QObject::tr("Rename..."), "Rename %1", [](const PrimvarCommandContext& context) {
        const std::optional<QString> to =
            askText(context, QObject::tr("Rename %1").arg(qs(context.primvar)), QObject::tr("New name:"), qs(context.primvar));
        if (!to || to->isEmpty() || to->toStdString() == context.primvar) return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>();
        const std::string from = context.primvar, name = to->toStdString();
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>(
            [from, name](const UsdPrim& prim) { return renamePrimvar(prim, from, name); });
    }));
    commands.push_back(onEach("usdcc.duplicate", QObject::tr("Duplicate..."), "Duplicate %1", [](const PrimvarCommandContext& context) {
        const std::optional<QString> to = askText(context, QObject::tr("Duplicate %1").arg(qs(context.primvar)), QObject::tr("The copy's name:"),
                                                  qs(context.primvar) + QStringLiteral("1"));
        if (!to || to->isEmpty()) return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>();
        const std::string from = context.primvar, name = to->toStdString();
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>(
            [from, name](const UsdPrim& prim) { return duplicatePrimvar(prim, from, name); });
    }));
    commands.push_back(onEach("usdcc.remove", QObject::tr("Remove"), "Remove %1", [](const PrimvarCommandContext& context) {
        const std::string name = context.primvar;
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name](const UsdPrim& prim) { return removePrimvar(prim, name); });
    }));
    commands.push_back(onEach("usdcc.block", QObject::tr("Block"), "Block %1", [](const PrimvarCommandContext& context) {
        const std::string name = context.primvar;
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name](const UsdPrim& prim) { return blockPrimvar(prim, name); });
    }));

    PrimvarCommand type = onEach("usdcc.type", QObject::tr("Change Type..."), "Change %1's Type", [](const PrimvarCommandContext& context) {
        QStringList items;
        if (const UsdGeomPrimvar primvar = firstPrimvar(context)) {
            for (const SdfValueTypeName& each : primvarTypesLike(primvar.GetTypeName())) items << qs(each.GetAsToken().GetString());
        }
        const std::optional<QString> chosen =
            items.isEmpty() ? std::nullopt : askItem(context, QObject::tr("Change %1's Type").arg(qs(context.primvar)), QObject::tr("Type:"), items);
        if (!chosen) return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>();
        const std::string name = context.primvar;
        const SdfValueTypeName to = SdfSchema::GetInstance().FindType(chosen->toStdString());
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name, to](const UsdPrim& prim) { return setPrimvarType(prim, name, to); });
    });
    type.disabled = [](const PrimvarCommandContext& context) -> std::optional<QString> {
        const UsdGeomPrimvar primvar = firstPrimvar(context);
        if (primvar && primvarTypesLike(primvar.GetTypeName()).empty()) {
            return QObject::tr("No other type holds %1's data").arg(qs(primvar.GetTypeName().GetAsToken().GetString()));
        }
        return std::nullopt;
    };
    commands.push_back(std::move(type));

    for (const TfToken& interpolation :
         {UsdGeomTokens->constant, UsdGeomTokens->uniform, UsdGeomTokens->varying, UsdGeomTokens->vertex, UsdGeomTokens->faceVarying}) {
        PrimvarCommand command = onEach(("usdcc.interpolation." + interpolation.GetString()).c_str(),
                                        QObject::tr("Interpolation/%1").arg(qs(interpolation.GetString())), "Make %1 ",
                                        [interpolation](const PrimvarCommandContext& context) {
                                            const std::string name = context.primvar;
                                            return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name, interpolation](const UsdPrim& prim) {
                                                return setPrimvarInterpolation(prim, name, interpolation);
                                            });
                                        });
        // The step's name says to what: "Make st vertex".
        const QString to = qs(interpolation.GetString());
        command.run = [interpolation, to](const PrimvarCommandContext& context) {
            const std::string name = context.primvar;
            runPrimvarEdit(context, QObject::tr("Make %1 %2").arg(qs(name), to), context.primsWith(),
                           [name, interpolation](const UsdPrim& prim) { return setPrimvarInterpolation(prim, name, interpolation); });
        };
        command.disabled = [interpolation](const PrimvarCommandContext& context) -> std::optional<QString> {
            const UsdGeomPrimvar primvar = firstPrimvar(context);
            if (primvar && primvar.GetInterpolation() == interpolation) return QObject::tr("It's %1 already").arg(qs(interpolation.GetString()));
            return std::nullopt;
        };
        commands.push_back(std::move(command));
    }

    PrimvarCommand index = onEach("usdcc.index", QObject::tr("Add Indices"), "Index %1", [](const PrimvarCommandContext& context) {
        const std::string name = context.primvar;
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name](const UsdPrim& prim) { return indexPrimvar(prim, name); });
    });
    index.applies = [](const PrimvarCommandContext& context) {
        const UsdGeomPrimvar primvar = firstPrimvar(context);
        return primvar && !primvar.IsIndexed() && primvar.GetTypeName().IsArray();
    };
    commands.push_back(std::move(index));
    PrimvarCommand flatten = onEach("usdcc.flatten", QObject::tr("Flatten Indices"), "Flatten %1", [](const PrimvarCommandContext& context) {
        const std::string name = context.primvar;
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name](const UsdPrim& prim) { return flattenPrimvarIndices(prim, name); });
    });
    flatten.applies = [](const PrimvarCommandContext& context) {
        const UsdGeomPrimvar primvar = firstPrimvar(context);
        return primvar && primvar.IsIndexed();
    };
    commands.push_back(std::move(flatten));

    PrimvarCommand size = onEach("usdcc.elementSize", QObject::tr("Element Size..."), "Set %1's Element Size", [](const PrimvarCommandContext& context) {
        const UsdGeomPrimvar primvar = firstPrimvar(context);
        const std::optional<int> chosen = askNumber(context, QObject::tr("%1's Element Size").arg(qs(context.primvar)),
                                                    QObject::tr("Values in each element:"), primvar ? std::max(primvar.GetElementSize(), 1) : 1, 1, 1024);
        if (!chosen) return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>();
        const std::string name = context.primvar;
        const int to = *chosen;
        return std::optional<std::function<PrimvarEdit(const UsdPrim&)>>([name, to](const UsdPrim& prim) { return setPrimvarElementSize(prim, name, to); });
    });
    size.applies = [](const PrimvarCommandContext& context) {
        const UsdGeomPrimvar primvar = firstPrimvar(context);
        return primvar && primvar.GetTypeName().IsArray();
    };
    commands.push_back(std::move(size));

    // From the first selected prim that has it to the other selected prims.
    PrimvarCommand copy;
    copy.id = "usdcc.copyToSelected";
    copy.label = QObject::tr("Copy to Other Selected Prims");
    copy.applies = [](const PrimvarCommandContext& context) { return !context.primvar.empty() && context.prims.size() > 1; };
    copy.run = [](const PrimvarCommandContext& context) {
        const std::vector<SdfPath> with = context.primsWith();
        if (with.empty()) return;
        const SdfPath from = with.front();
        std::vector<SdfPath> to;
        for (const SdfPath& path : context.prims) {
            if (path != from) to.push_back(path);
        }
        const std::string name = context.primvar;
        const StageRefPtr stage = context.stage;
        runPrimvarEdit(context, QObject::tr("Copy %1 from %2").arg(qs(name), qs(from.GetName())), to, [stage, from, name](const UsdPrim& prim) {
            return copyPrimvar(stage->usdStage()->GetPrimAtPath(from), prim, name);
        });
    };
    commands.push_back(std::move(copy));

    PrimvarCommand create;
    create.id = "usdcc.create";
    create.label = QObject::tr("Create Primvar...");
    create.applies = [](const PrimvarCommandContext& context) { return context.stage && !context.prims.empty(); };
    create.run = [](const PrimvarCommandContext& context) {
        QStringList names;
        for (const SdfPath& path : context.prims) names << qs(path.GetString());
        const std::optional<PrimvarCreateRequest> request = askCreate(context, names);
        if (!request) return;
        runPrimvarEdit(context, QObject::tr("Create %1").arg(qs(request->name)), context.prims, [request](const UsdPrim& prim) {
            return createPrimvar(prim, request->name, request->type, request->interpolation, request->elementSize);
        });
    };
    commands.push_back(std::move(create));
}

}  // namespace

std::vector<SdfPath> PrimvarCommandContext::primsWith(const std::string& name) const {
    std::vector<SdfPath> found;
    const std::string wanted = name.empty() ? primvar : name;
    if (!stage || wanted.empty()) return found;
    for (const SdfPath& path : prims) {
        const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
        if (prim && UsdGeomPrimvarsAPI(prim).HasPrimvar(TfToken(wanted))) found.push_back(path);
    }
    return found;
}

PrimvarCommands& PrimvarCommands::instance() {
    static PrimvarCommands* const commands = new PrimvarCommands;  // outlives the plugins that add to it
    return *commands;
}

PrimvarCommands::PrimvarCommands() { reset(); }

void PrimvarCommands::reset() {
    m_commands.clear();
    addBuiltIns(m_commands);
}

void PrimvarCommands::add(PrimvarCommand command) {
    const auto it = std::find_if(m_commands.begin(), m_commands.end(), [&](const PrimvarCommand& other) { return other.id == command.id; });
    if (it != m_commands.end()) {
        *it = std::move(command);
    } else {
        m_commands.push_back(std::move(command));
    }
}

bool PrimvarCommands::remove(const std::string& id) {
    const auto it = std::find_if(m_commands.begin(), m_commands.end(), [&](const PrimvarCommand& command) { return command.id == id; });
    if (it == m_commands.end()) return false;
    m_commands.erase(it);
    return true;
}

const PrimvarCommand* PrimvarCommands::find(const std::string& id) const {
    const auto it = std::find_if(m_commands.begin(), m_commands.end(), [&](const PrimvarCommand& command) { return command.id == id; });
    return it == m_commands.end() ? nullptr : &*it;
}

void addPrimvarCommands(QMenu* menu, const PrimvarCommandContext& context) {
    if (!menu) return;
    for (const PrimvarCommand& command : PrimvarCommands::instance().commands()) {
        const bool applies = command.applies ? command.applies(context) : !context.primvar.empty();
        if (!applies || !command.run) continue;
        QMenu* parent = menu;
        const QStringList parts = command.label.split(QLatin1Char('/'));
        for (int i = 0; i + 1 < parts.size(); ++i) {
            QMenu* found = nullptr;
            for (QAction* action : parent->actions()) {
                if (action->menu() && action->text() == parts[i]) found = action->menu();
            }
            parent = found ? found : parent->addMenu(parts[i]);
        }
        QAction* action = parent->addAction(parts.back());
        action->setObjectName(QString::fromStdString(command.id));
        if (const std::optional<QString> why = command.disabled ? command.disabled(context) : std::nullopt) {
            action->setEnabled(false);
            action->setToolTip(*why);
        }
        const std::string id = command.id;
        QObject::connect(action, &QAction::triggered, menu, [id, context]() {
            // Found again: the menu may outlive a plugin's command.
            if (const PrimvarCommand* found = PrimvarCommands::instance().find(id)) found->run(context);
        });
    }
    menu->setToolTipsVisible(true);
}

PrimvarEdit runPrimvarEdit(const PrimvarCommandContext& context, const QString& what, const std::vector<SdfPath>& prims,
                           const std::function<PrimvarEdit(const UsdPrim&)>& edit) {
    PrimvarEdit total;
    if (!context.stage || prims.empty()) {
        total.error = context.primvar.empty() ? "Nothing is selected." : "None of the selected prims has " + context.primvar + ".";
    } else {
        const auto body = [&]() {
            for (const SdfPath& path : prims) {
                const UsdPrim prim = context.stage->usdStage()->GetPrimAtPath(path);
                const PrimvarEdit one = edit(prim);
                const std::string who = prims.size() > 1 ? path.GetName() + ": " : std::string();
                if (!one) {
                    total.error = who + one.error;
                    return false;
                }
                for (const std::string& note : one.notes) total.notes.push_back(who + note);
            }
            return true;
        };
        total.ok = context.undo ? context.undo->apply(context.stage, what, body) : body();
        if (!total.ok) total.notes.clear();
    }
    if (context.report) context.report(what, total);
    return total;
}

std::vector<SdfValueTypeName> primvarCreateTypes() {
    const auto& t = SdfValueTypeNames;
    return {t->FloatArray,   t->Float2Array,   t->TexCoord2fArray, t->Float3Array, t->Color3fArray, t->Normal3fArray, t->Vector3fArray,
            t->Point3fArray, t->Float4Array,   t->Color4fArray,    t->IntArray,    t->Int2Array,    t->BoolArray,     t->StringArray,
            t->TokenArray,   t->DoubleArray,   t->Float,           t->Float3,      t->Color3f,      t->Int,           t->String,
            t->Token};
}

}  // namespace usdcc::usd
