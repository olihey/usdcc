#pragma once

// Refreshing the configurations that come from a URL (docs/ASSET_SOURCES.md §7).

#include "usdcc/core/export.h"

#include <QObject>
#include <QTimer>

#include <vector>

namespace usdcc::assets {
struct SourceEntry;
}

namespace usdcc::usd {

// Asks each source's server in the background, at start and every hour. When one changed, the
// library's configuration is read again and assetConfigChanged() fires: stages opened from
// then on use it.
class USDCC_USD_UI_API AssetSourceRefresher : public QObject {
    Q_OBJECT

public:
    explicit AssetSourceRefresher(QObject* parent = nullptr);

    // The first refresh now, then every hour.
    void start();
    // Now, unless one is under way or usdcc works offline.
    void refresh();
    bool busy() const { return m_busy; }

Q_SIGNALS:
    void finished(bool changed);

private:
    QTimer m_hourly;
    bool m_busy = false;
};

}  // namespace usdcc::usd
