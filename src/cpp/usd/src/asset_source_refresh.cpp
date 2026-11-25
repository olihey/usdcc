#include "usdcc/usd/asset_source_refresh.h"

#include "usdcc/assets/activity.h"
#include "usdcc/assets/config.h"
#include "usdcc/assets/context.h"
#include "usdcc/assets/sources.h"
#include "usdcc/core/log.h"
#include "usdcc/usd/asset_servers.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>

#include <chrono>

namespace usdcc::usd {

AssetSourceRefresher::AssetSourceRefresher(QObject* parent) : QObject(parent) {
    m_hourly.setInterval(60 * 60 * 1000);
    connect(&m_hourly, &QTimer::timeout, this, &AssetSourceRefresher::refresh);
}

void AssetSourceRefresher::start() {
    m_hourly.start();
    refresh();
}

void AssetSourceRefresher::refresh() {
    if (m_busy || usdcc::assets::isOffline()) return;
    m_busy = true;
    QPointer<AssetSourceRefresher> self(this);
    QThreadPool::globalInstance()->start([self]() {
        bool changed = false;
        // As the file has them now: the merged configuration doesn't say.
        const usdcc::assets::Result<usdcc::assets::Config> personal =
            usdcc::assets::Config::load(usdcc::assets::Config::defaultPath());
        if (personal.ok()) {
            for (const usdcc::assets::SourceEntry& entry : personal.value.sources) {
                if (entry.url.empty()) continue;
                const usdcc::assets::SourceRefresh result =
                    usdcc::assets::refreshSource(entry, std::chrono::milliseconds(30000));
                if (result.what == usdcc::assets::Refreshed::Changed) {
                    changed = true;
                    USDCC_LOG_INFO("usdcc.assets", "The asset configuration {} changed on its server", entry.alias);
                } else if (result.what == usdcc::assets::Refreshed::Failed) {
                    USDCC_LOG_WARNING("usdcc.assets", "The asset configuration {} couldn't be refreshed: {}", entry.alias,
                                      result.problem);
                }
            }
        }
        QMetaObject::invokeMethod(
            qApp,
            [self, changed]() {
                if (!self) return;
                self->m_busy = false;
                if (changed) {
                    usdcc::assets::reloadDefaultConfig();
                    assetConfigChanged()();
                }
                Q_EMIT self->finished(changed);
            },
            Qt::QueuedConnection);
    });
}

}  // namespace usdcc::usd
