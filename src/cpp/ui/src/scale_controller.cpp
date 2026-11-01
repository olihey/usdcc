#include "usdcc/ui/scale_controller.h"

#include "usdcc/ui/settings.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QRegularExpression>
#include <QSettings>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace usdcc::ui {

namespace {
constexpr auto kScaleKey = "Appearance/uiScale";
}  // namespace

UiScaleController::UiScaleController(QApplication& app, const QString& settingsFile) : QObject(&app) {
    // Held open rather than opened per use like appSettings(): the file may be
    // a test's, and QSettings shares one cache per file within the process.
    m_settings = new QSettings(settingsFile.isEmpty() ? appSettings().fileName() : settingsFile,
                               QSettings::IniFormat, this);
    bool ok = false;
    const int saved = m_settings->value(kScaleKey, 100).toInt(&ok);
    m_percent = ok ? std::clamp(saved, minimumPercent, maximumPercent) : 100;
    app.installEventFilter(this);
}

UiScaleController& UiScaleController::instance() {
    if (auto* controller = qApp->findChild<UiScaleController*>(QString(), Qt::FindDirectChildrenOnly)) {
        return *controller;
    }
    return *new UiScaleController(*qApp);
}

int UiScaleController::pixels(int base) const {
    // Qt uses negative sizes/spacings as sentinels (e.g. -1 = use the style).
    return base < 0 ? base : qRound(base * m_percent / 100.0);
}

QString UiScaleController::styleSheet(const QString& original) const {
    if (m_percent == 100) return original;
    // Keep resource names and comments intact. Only CSS pixel dimensions are
    // scaled; colours, percentages and unitless values are not dimensions.
    static const QRegularExpression dimensions(
        QStringLiteral(R"((url\([^)]*\)|/\*[\s\S]*?\*/)|(-?\d+(?:\.\d+)?)px\b)"));
    QString result;
    qsizetype offset = 0;
    auto matches = dimensions.globalMatch(original);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += original.mid(offset, match.capturedStart() - offset);
        if (!match.captured(1).isEmpty()) {
            result += match.captured();
        } else {
            const double base = match.captured(2).toDouble();
            // Hairline borders remain one logical pixel at every density.
            const int scaled = qAbs(base) <= 1.0 ? qRound(base) : qRound(base * m_percent / 100.0);
            result += QString::number(scaled) + QStringLiteral("px");
        }
        offset = match.capturedEnd();
    }
    result += original.mid(offset);
    return result;
}

void UiScaleController::setPercent(int percent) {
    const int next = std::clamp(percent, minimumPercent, maximumPercent);
    if (next == m_percent) return;
    m_percent = next;
    m_settings->setValue(kScaleKey, m_percent);
    m_settings->sync();
    Q_EMIT scaleChanged(m_percent);
    for (QWidget* widget : QApplication::allWidgets()) {
        widget->updateGeometry();
        widget->update();
    }
}

void UiScaleController::bind(QObject* owner, const char* name, std::function<void()> apply) {
    const QString key = QStringLiteral("_usdccScale_") + QLatin1String(name);
    delete owner->findChild<QObject*>(key, Qt::FindDirectChildrenOnly);
    auto* binding = new QObject(owner);
    binding->setObjectName(key);
    connect(this, &UiScaleController::scaleChanged, binding, [apply](int) { apply(); });
    apply();
}

bool UiScaleController::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    const bool keypadControl = key->modifiers() == (Qt::ControlModifier | Qt::KeypadModifier);
    if (!keypadControl || (key->key() != Qt::Key_Plus && key->key() != Qt::Key_Minus)) {
        return QObject::eventFilter(watched, event);
    }
    // Reserve the combination before editors or QAction shortcuts can use it.
    event->accept();
    if (event->type() == QEvent::KeyPress) {
        setPercent(m_percent + (key->key() == Qt::Key_Plus ? stepPercent : -stepPercent));
    }
    return true;
}

void UiScaleController::addMenu(QMenu* viewMenu) {
    viewMenu->addSeparator();
    QMenu* menu = viewMenu->addMenu(QString());
    QAction* increase = menu->addAction(tr("Increase\tCtrl+Num+"), this,
                                       [this]() { setPercent(m_percent + stepPercent); });
    QAction* decrease = menu->addAction(tr("Decrease\tCtrl+Num-"), this,
                                       [this]() { setPercent(m_percent - stepPercent); });
    menu->addSeparator();
    QAction* reset = menu->addAction(tr("Reset to 100%"), this, [this]() { setPercent(100); });
    const auto refresh = [this, menu, increase, decrease, reset]() {
        menu->setTitle(tr("UI Scale (%1%)").arg(m_percent));
        increase->setEnabled(m_percent < maximumPercent);
        decrease->setEnabled(m_percent > minimumPercent);
        reset->setEnabled(m_percent != 100);
    };
    connect(this, &UiScaleController::scaleChanged, menu, [refresh](int) { refresh(); });
    refresh();
}

}  // namespace usdcc::ui
