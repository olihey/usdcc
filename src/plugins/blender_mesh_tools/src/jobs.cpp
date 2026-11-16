// SPDX-License-Identifier: BSD-3-Clause
#include "jobs.h"
#include "usdcc/ui/settings.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/undo_manager.h"
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <chrono>
#include <set>

namespace usdcc::blender_mesh {
QString operationName(int operation) {
    switch (operation) {
    case USDCC_MESH_TRIANGULATE: return QStringLiteral("Triangulate");
    case USDCC_MESH_MERGE: return QStringLiteral("Merge by Distance");
    case USDCC_MESH_DISSOLVE: return QStringLiteral("Limited Dissolve");
    case USDCC_MESH_NORMALS: return QStringLiteral("Recalculate Normals");
    case USDCC_MESH_DECIMATE: return QStringLiteral("Decimate");
    case USDCC_MESH_BEVEL: return QStringLiteral("Bevel");
    default: return QStringLiteral("Mesh operation");
    }
}
Jobs::Jobs(plugin::Host& host, QObject* parent) : QObject(parent), m_host(host) {
    setObjectName(QStringLiteral("BlenderMeshJobs"));
}
Jobs::~Jobs() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
    m_changes.disconnect();
    if (m_progress) delete m_progress;
    usdcc_blender_mesh_shutdown();
}
void Jobs::cancel() { if (m_cancel) m_cancel->store(true); }
void Jobs::report(core::LogLevel level, const std::string& message) {
    m_status = message;
    setProperty("status", QString::fromStdString(message));
    core::LogRecord record;
    record.level = level; record.source = "plugin:" + m_host.id;
    record.category = "plugin." + m_host.id; record.message = message;
    core::ToastRequest toast;
    toast.title = m_name.toStdString(); toast.body = message;
    record.toast = toast;
    core::LogHub::instance().post(std::move(record));
}
bool Jobs::start(const usd::StageRefPtr& stage, const std::vector<PXR_NS::SdfPath>& paths, UsdccMeshOptions options) {
    if (busy()) { report(core::LogLevel::Warning, "A mesh job is already running."); return false; }
    m_name = operationName(options.operation);
    if (!stage || !m_host.stageManager || !m_host.undoManager || paths.empty()) {
        report(core::LogLevel::Warning, "Select one or more polygon meshes."); return false;
    }
    std::vector<Snapshot> inputs;
    std::string error;
    size_t corners = 0, points = 0, floats = 0;
    const std::set<PXR_NS::SdfPath> unique(paths.begin(), paths.end());
    for (const auto& path : unique) {
        Snapshot input;
        if (!capture(stage->usdStage(), path, input, error)) {
            report(core::LogLevel::Warning, path.GetString() + ": " + error); return false;
        }
        corners += input.mesh.indices.size(); points += input.mesh.points.size() / 3;
        floats += input.mesh.points.size();
        for (const auto& c : input.mesh.channels) floats += c.values.size();
        if (corners > USDCC_MESH_MAX_CORNERS || points > USDCC_MESH_MAX_POINTS || floats > 64000000) {
            report(core::LogLevel::Warning, "Selection exceeds the mesh job memory budget."); return false;
        }
        inputs.push_back(std::move(input));
    }
    m_inputs = std::move(inputs); m_stage = stage; m_target = stage->usdStage()->GetEditTarget();
    m_stale = false;
    m_changes = stage->changed.connect([this](const auto&) { m_stale = true; });
    m_cancel = std::make_shared<std::atomic_bool>(false);
    setProperty("busy", true); setProperty("status", QStringLiteral("Running"));
    auto cancel = m_cancel;
    options.cancelled = [](void* token) { return static_cast<std::atomic_bool*>(token)->load() ? 1 : 0; };
    options.cancellation = cancel.get();
    if (m_host.mainWindow) {
        m_progress = new QProgressDialog(m_name + QStringLiteral("…"), QStringLiteral("Cancel"), 0, 0, m_host.mainWindow);
        m_progress->setObjectName(QStringLiteral("BlenderMeshProgress"));
        m_progress->setWindowModality(Qt::NonModal);
        m_progress->setMinimumDuration(0);
        connect(m_progress, &QProgressDialog::canceled, this, [this] { this->cancel(); });
        m_progress->show();
    }
    // Worker receives only plain mesh buffers, parameters and cancellation.
    std::vector<MeshData> meshes;
    for (const auto& input : m_inputs) meshes.push_back(input.mesh);
    m_thread = std::thread([this, meshes = std::move(meshes), options, cancel]() mutable {
        std::vector<Result> results;
        std::string error;
        try {
            for (const auto& mesh : meshes) {
                Result result;
                if (!execute(mesh, options, result, error)) break;
                results.push_back(std::move(result));
            }
        } catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "Unknown mesh worker failure"; }
        QMetaObject::invokeMethod(this, [this, results = std::move(results), error = std::move(error), cancel]() mutable {
            finish(std::move(results), std::move(error), cancel);
        }, Qt::QueuedConnection);
    });
    return true;
}
void Jobs::finish(std::vector<Result> results, std::string error, std::shared_ptr<std::atomic_bool> cancel) {
    if (m_thread.joinable()) m_thread.join();
    if (m_progress) { delete m_progress; m_progress = nullptr; }
    const auto stage = m_stage;
    const auto& open = m_host.stageManager->stages();
    if (cancel->load()) error = "Cancelled; no geometry was changed.";
    else if (std::find(open.begin(), open.end(), stage) == open.end()) error = "Stage closed; result discarded.";
    else if (m_stale || stage->usdStage()->GetEditTarget() != m_target) error = "Stage or edit target changed; result discarded.";
    else if (m_host.undoManager->isEditing(stage))
        error = "Another edit is recording this layer; result discarded.";
    if (error.empty() && results.size() != m_inputs.size()) error = "Mesh worker returned an incomplete batch.";
    bool anyChanges = false;
    for (size_t i = 0; error.empty() && i < results.size(); ++i) {
        Snapshot current;
        if (!capture(stage->usdStage(), m_inputs[i].path, current, error)) break;
        if (!sameSnapshot(m_inputs[i], current)) { error = "Mesh input changed; result discarded."; break; }
        if (!validateResult(m_inputs[i], results[i], error)) break;
        anyChanges |= changed(m_inputs[i], results[i]);
    }
    // Disconnect before our own authoring notices. Commit starts a fresh session,
    // after Host's synchronous menu scope has ended, and verifies composition.
    m_changes.disconnect();
    if (error.empty() && anyChanges) {
        auto& undo = *m_host.undoManager;
        if (!undo.tryBeginEdit(stage, m_name)) error = "Another edit is recording this layer; result discarded.";
        else try {
            for (size_t i = 0; i < results.size(); ++i) if (changed(m_inputs[i], results[i])) {
                if (!author(stage->usdStage(), m_inputs[i], results[i], error)) break;
            }
            if (error.empty()) {
                if (!undo.endEdit(stage)) error = "Geometry could not be recorded for undo.";
            } else undo.cancelEdit(stage);
        } catch (const std::exception& e) { undo.cancelEdit(stage); error = e.what(); }
        catch (...) { undo.cancelEdit(stage); error = "Unknown mesh commit failure; rolled back."; }
    }
    m_cancel.reset(); m_stage.reset(); m_inputs.clear();
    setProperty("busy", false);
    if (!error.empty()) report(core::LogLevel::Warning, error);
    else {
        std::string message = anyChanges ? "Mesh edit committed as one undo step." : "No geometry changes were needed.";
        for (const auto& result : results) if (!result.diagnostic.empty()) message += " " + result.diagnostic;
        report(core::LogLevel::Info, message);
    }
}
bool chooseOptions(plugin::Host& host, int operation, UsdccMeshOptions& options, const usd::StageRefPtr& stage,
                   const std::vector<PXR_NS::SdfPath>& paths) {
    QDialog dialog(host.mainWindow);
    dialog.setObjectName(QStringLiteral("BlenderMeshOptions"));
    dialog.setWindowTitle(operationName(operation));
    auto* form = new QFormLayout(&dialog);
    auto* description = new QLabel(QStringLiteral("Whole selected polygon meshes. Distances use mesh-local units.\n"
        "The result stays on the same USD prims; changed geometry gets flat face-varying normals."));
    description->setWordWrap(true); form->addRow(description);
    auto settings = ui::appSettings();
    const auto key = host.settingKey(QStringLiteral("Mesh/%1/").arg(operation));
    auto spin = [&](const QString& name, const QString& label, double value, double lo, double hi, int decimals) {
        auto* box = new QDoubleSpinBox(&dialog); box->setObjectName(name); box->setDecimals(decimals); box->setRange(lo, hi);
        box->setValue(settings.value(key + name, value).toDouble()); form->addRow(label, box); return box;
    };
    QDoubleSpinBox *distance = nullptr, *angle = nullptr, *ratio = nullptr, *profile = nullptr;
    QSpinBox* segments = nullptr;
    QComboBox* material = nullptr;
    QCheckBox* clamp = nullptr;
    QComboBox *quads = nullptr, *ngons = nullptr;
    options = defaultOptions(operation);
    if (operation == USDCC_MESH_MERGE || operation == USDCC_MESH_BEVEL)
        distance = spin("distance", operation == USDCC_MESH_MERGE ? "Distance (local units)" : "Width (local units)", options.distance, 0, 1000000, 6);
    if (operation == USDCC_MESH_DISSOLVE || operation == USDCC_MESH_BEVEL)
        angle = spin("angle", operation == USDCC_MESH_DISSOLVE ? "Maximum angle (degrees)" : "Minimum edge angle (degrees)", options.angle * 180 / 3.141592653589793, 0, 180, 2);
    if (operation == USDCC_MESH_DECIMATE) {
        ratio = spin("ratio", "Target face ratio", options.ratio, 0.001, 1, 3);
        auto* label = new QLabel("Requires a closed mesh with no protected UV, sharp-normal or material boundaries. The achieved face count is reported.");
        label->setWordWrap(true); form->addRow(label);
    }
    if (operation == USDCC_MESH_BEVEL) {
        profile = spin("profile", "Profile", options.profile, 0.01, 0.99, 2);
        segments = new QSpinBox(&dialog); segments->setObjectName("segments"); segments->setRange(1,16);
        segments->setValue(settings.value(key + "segments", 1).toInt()); form->addRow("Segments", segments);
        material = new QComboBox(&dialog); material->setObjectName("material");
        material->addItem("Adjacent face material", -1);
        const std::set<PXR_NS::SdfPath> unique(paths.begin(), paths.end());
        if (stage && unique.size() == 1) {
            auto mesh = PXR_NS::UsdGeomMesh(stage->usdStage()->GetPrimAtPath(*unique.begin()));
            if (mesh) {
                const auto subsets = PXR_NS::UsdGeomSubset::GetAllGeomSubsets(mesh);
                for (size_t i = 0; i < subsets.size(); ++i)
                    material->addItem(QString::fromStdString(subsets[i].GetPrim().GetName().GetString()), int(i));
            }
        }
        const int saved = material->findText(settings.value(key + "materialName", "Adjacent face material").toString());
        if (saved >= 0) material->setCurrentIndex(saved);
        form->addRow("New-face material", material);
        clamp = new QCheckBox("Clamp overlap", &dialog); clamp->setChecked(settings.value(key + "clamp", true).toBool()); form->addRow(clamp);
    }
    if (operation == USDCC_MESH_TRIANGULATE) {
        quads = new QComboBox(&dialog); quads->setObjectName("quadMethod");
        quads->addItems({"Fixed", "Alternate", "Shortest diagonal", "Longest diagonal", "Beauty"});
        quads->setCurrentIndex(settings.value(key + "quads", 4).toInt()); form->addRow("Quads", quads);
        ngons = new QComboBox(&dialog); ngons->addItems({"Beauty", "Ear clip"}); ngons->setCurrentIndex(settings.value(key + "ngons", 0).toInt()); form->addRow("N-gons", ngons);
    }
    if (operation == USDCC_MESH_NORMALS) {
        auto* label = new QLabel("Makes connected winding consistent where possible and writes flat face-varying normals. Outside orientation of open/non-manifold components is reported as ambiguous.");
        label->setWordWrap(true); form->addRow(label);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply"); form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    if (distance) { options.distance = float(distance->value()); settings.setValue(key + "distance", distance->value()); }
    if (angle) { options.angle = float(angle->value() * 3.141592653589793 / 180); settings.setValue(key + "angle", angle->value()); }
    if (ratio) { options.ratio = float(ratio->value()); settings.setValue(key + "ratio", ratio->value()); }
    if (profile) { options.profile = float(profile->value()); settings.setValue(key + "profile", profile->value()); }
    if (segments) { options.segments = segments->value(); settings.setValue(key + "segments", segments->value()); }
    if (material) { options.bevel_material = material->currentData().toInt(); settings.setValue(key + "materialName", material->currentText()); }
    if (clamp) { options.clamp_overlap = clamp->isChecked(); settings.setValue(key + "clamp", clamp->isChecked()); }
    if (quads) { options.quad_method = quads->currentIndex(); settings.setValue(key + "quads", options.quad_method); }
    if (ngons) { options.ngon_method = ngons->currentIndex(); settings.setValue(key + "ngons", options.ngon_method); }
    return true;
}
}
