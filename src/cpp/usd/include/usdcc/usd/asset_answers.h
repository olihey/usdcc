#pragma once

#include "usdcc/core/export.h"

#include <QObject>
#include <QTimer>

namespace usdcc::usd {

// Tells panels that servers answered what they were shown as resolving, or differently than
// before (usdcc/assets/no_wait.h). On the UI thread, several answers at once.
class USDCC_USD_UI_API AssetAnswers : public QObject {
    Q_OBJECT

public:
    // Made on first use, which is on the UI thread; goes with the QCoreApplication.
    static AssetAnswers& instance();
    ~AssetAnswers() override;

Q_SIGNALS:
    void changed();

private:
    explicit AssetAnswers(QObject* parent);

    QTimer m_timer;
};

}  // namespace usdcc::usd
