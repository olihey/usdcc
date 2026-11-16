// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "conversion.h"
#include "usdcc/plugin/plugin.h"
#include "usdcc/usd/stage.h"
#include <QObject>
#include <QPointer>
#include <atomic>
#include <thread>

class QProgressDialog;
namespace usdcc::blender_mesh {
QString operationName(int operation);
class Jobs final : public QObject {
public:
    explicit Jobs(plugin::Host&, QObject* parent = nullptr);
    ~Jobs() override;
    bool start(const usd::StageRefPtr&, const std::vector<PXR_NS::SdfPath>&, UsdccMeshOptions);
    bool busy() const { return bool(m_cancel); }
    void cancel();
    std::string status() const { return m_status; }
private:
    void finish(std::vector<Result>, std::string, std::shared_ptr<std::atomic_bool>);
    void report(core::LogLevel, const std::string&);
    plugin::Host& m_host;
    std::thread m_thread;
    std::shared_ptr<std::atomic_bool> m_cancel;
    usd::StageRefPtr m_stage;
    PXR_NS::UsdEditTarget m_target;
    std::vector<Snapshot> m_inputs;
    core::Signal<const usd::StageChanges&>::Connection m_changes;
    bool m_stale = false;
    QString m_name;
    QPointer<QProgressDialog> m_progress;
    std::string m_status;
};
// Dialogs are synchronous only for choosing options; computation is asynchronous.
bool chooseOptions(plugin::Host&, int operation, UsdccMeshOptions&, const usd::StageRefPtr&,
                   const std::vector<PXR_NS::SdfPath>&);
}
