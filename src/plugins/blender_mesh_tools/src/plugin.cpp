// SPDX-License-Identifier: BSD-3-Clause
#include "usdcc/plugin/plugin.h"
#include "probe_api.h"
#include "jobs.h"
#include "usdcc/usd/stage_manager.h"
#include <pxr/usd/usdGeom/mesh.h>
#include <QPointer>
#include <QVariant>
#include <unordered_map>
#include <stdexcept>
#include <string>

namespace {
std::unordered_map<usdcc::plugin::Host*, std::unique_ptr<usdcc::blender_mesh::Jobs>> jobs;
void probe(usdcc::plugin::Host& host, unsigned iterations) {
    char report[512];
    if (usdcc_blender_mesh_probe(iterations, report, sizeof(report))) {
        throw std::runtime_error(std::string("Blender mesh probe failed: ") + report);
    }
    host.log(usdcc::core::LogLevel::Info, std::string(usdcc_blender_mesh_build()) + " " + report);
}
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_activate(usdcc::plugin::Host& host) {
    probe(host, 10);
    host.addMenuAction("Debug/Blender Mesh Backend Probe", [&host] { probe(host, 100); });
    auto controller = std::make_unique<usdcc::blender_mesh::Jobs>(host);
    const QPointer<usdcc::blender_mesh::Jobs> weak(controller.get());
    if (host.mainWindow) host.mainWindow->setProperty("BlenderMeshJobs", QVariant::fromValue(static_cast<QObject*>(controller.get())));
    for (int operation = USDCC_MESH_TRIANGULATE; operation <= USDCC_MESH_BEVEL; ++operation) {
        const auto name = usdcc::blender_mesh::operationName(operation);
        host.addMenuAction(QStringLiteral("Mesh/") + name + QStringLiteral("…"), [&host, weak, operation] {
            if (!weak || !host.stageManager) return;
            const auto stage = host.stageManager->currentStage();
            UsdccMeshOptions options{};
            if (usdcc::blender_mesh::chooseOptions(host, operation, options, stage, stage ? stage->selectedPaths() : std::vector<PXR_NS::SdfPath>{}))
                weak->start(stage, stage ? stage->selectedPaths() : std::vector<PXR_NS::SdfPath>{}, options);
        });
        host.addPrimMenuAction(QStringLiteral("Mesh/") + name + QStringLiteral("…"),
            [&host, weak, operation](const auto& context) {
                if (!weak) return;
                UsdccMeshOptions options{};
                if (usdcc::blender_mesh::chooseOptions(host, operation, options, context.stage, context.paths)) weak->start(context.stage, context.paths, options);
            }, [](const auto& context) {
                return context.stage && !context.paths.empty() && std::all_of(context.paths.begin(), context.paths.end(), [&](const auto& path) {
                    return bool(PXR_NS::UsdGeomMesh(context.stage->usdStage()->GetPrimAtPath(path)));
                });
            });
    }
    jobs[&host] = std::move(controller);
    if (host.mainWindow) QObject::connect(host.mainWindow, &QObject::destroyed, [key = &host, weak] {
        if (weak) jobs.erase(key);
    });
    host.log(usdcc::core::LogLevel::Info, "native mesh backend active; mesh commands enabled");
}

USDCC_PLUGIN_EXPORT void usdcc_plugin_deactivate(usdcc::plugin::Host& host) {
    if (jobs.contains(&host) && host.mainWindow) host.mainWindow->setProperty("BlenderMeshJobs", QVariant{});
    jobs.erase(&host); // cancel, join, and tear down the persistent backend worker
    for (int operation = USDCC_MESH_TRIANGULATE; operation <= USDCC_MESH_BEVEL; ++operation)
        usdcc::usd::PrimMenuEntries::instance().remove(host.id + ".Mesh/" + usdcc::blender_mesh::operationName(operation).toStdString() + "…");
    // Verify a fresh worker/runtime cycle during the plugin's shutdown hook.
    probe(host, 1);
    host.log(usdcc::core::LogLevel::Info, "native mesh backend shutdown probe passed");
    usdcc_blender_mesh_shutdown();
}
