#pragma once

// The system's proxy, for the requests of one QNetworkAccessManager alone
// (setProxyFactory(new SystemProxy)): the update check and the packs' downloads.

#include <QNetworkProxy>

namespace usdcc::app {

class SystemProxy : public QNetworkProxyFactory {
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override { return systemProxyForQuery(query); }
};

}  // namespace usdcc::app
