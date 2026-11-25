#include "usdcc/usd/asset_answers.h"

#include "usdcc/assets/no_wait.h"

#include <QCoreApplication>
#include <QPointer>

namespace usdcc::usd {

AssetAnswers& AssetAnswers::instance() {
    static QPointer<AssetAnswers> instance;
    if (!instance) instance = new AssetAnswers(QCoreApplication::instance());
    return *instance;
}

AssetAnswers::AssetAnswers(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &AssetAnswers::changed);
    // From a worker. This is there for as long as it listens: the destructor waits.
    usdcc::assets::setAnswerListener([this]() {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                if (!m_timer.isActive()) m_timer.start();
            },
            Qt::QueuedConnection);
    });
}

AssetAnswers::~AssetAnswers() { usdcc::assets::stopAsking(); }

}  // namespace usdcc::usd
