#include "usdcc/ui/theme.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scale_controller.h"

#include <DockManager.h>

#include <QApplication>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPalette>
#include <QPixmap>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QVariant>

// Q_INIT_RESOURCE must be used outside any namespace, and is required here
// because the resources live in a static library — without it the linker can
// drop them and the stylesheets silently load empty.
static void initThemeResources() { Q_INIT_RESOURCE(usdcc_ui); }

namespace usdcc::ui::theme {

namespace {

QString readResource(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

// Same surfaces/text/accent as dark.qss — the palette is what widgets the
// stylesheet doesn't cover (and Fusion's own drawing) fall back to.
QPalette darkPalette(const QColor& accent) {
    const QColor panel(0x1c, 0x1c, 0x1e);
    const QColor inset(0x14, 0x14, 0x16);
    const QColor text(0xe8, 0xe8, 0xea);
    const QColor disabledText(255, 255, 255, 80);

    QPalette p;
    p.setColor(QPalette::Window, panel);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, inset);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x23, 0x23, 0x26));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0x23, 0x23, 0x26));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(255, 255, 255, 100));
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::LinkVisited, accent.lighter(130));

    // ADS's own rules reference these (palette(light) / palette(dark)).
    p.setColor(QPalette::Light, QColor(0x2a, 0x2a, 0x2e));
    p.setColor(QPalette::Midlight, QColor(0x24, 0x24, 0x28));
    p.setColor(QPalette::Mid, QColor(0x33, 0x33, 0x37));
    p.setColor(QPalette::Dark, QColor(0x0e, 0x0e, 0x10));
    p.setColor(QPalette::Shadow, Qt::black);

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    return p;
}

// The clear button of line edits (setClearButtonEnabled): a rounded square
// with the X cut out of it, square like the inputs it sits in rather than
// Fusion's round bitmap. Drawn as a path at whatever size and device pixel
// ratio is asked for, so it stays sharp at every UI scale.
class ClearIconEngine : public QIconEngine {
public:
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        const qreal side = qMin(rect.width(), rect.height()) * 0.75;
        QRectF box(0, 0, side, side);
        box.moveCenter(QRectF(rect).center());
        QPainterPath shape;
        shape.addRoundedRect(box, side * 0.16, side * 0.16);
        const qreal arm = side * 0.27;
        QPainterPath cross;
        cross.moveTo(box.center() + QPointF(-arm, -arm));
        cross.lineTo(box.center() + QPointF(arm, arm));
        cross.moveTo(box.center() + QPointF(arm, -arm));
        cross.lineTo(box.center() + QPointF(-arm, arm));
        QPainterPathStroker stroker;
        stroker.setWidth(qMax(1.0, side * 0.13));
        stroker.setCapStyle(Qt::FlatCap);
        shape = shape.subtracted(stroker.createStroke(cross));

        const int alpha = mode == QIcon::Disabled ? 50 : mode == QIcon::Active ? 190 : 115;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillPath(shape, QColor(255, 255, 255, alpha));
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap result(size * scale);
        result.setDevicePixelRatio(scale);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return result;
    }

    QIconEngine* clone() const override { return new ClearIconEngine; }
};

// Fusion, plus what the stylesheet can't reach: the clear button's icon and
// its size, which Qt takes from pixelMetric() in device-independent pixels and
// so would otherwise ignore the UI scale.
class Style : public QProxyStyle {
public:
    Style() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override {
        if (metric == PM_LineEditIconMargin && widget) {
            // QLineEdit centres the clear button, (icon + 6) x (icon + 2),
            // vertically and puts it this far from the right edge; the icon
            // has 3 px either side of it in the button. Taking the margin from
            // the field's height makes the gap on the right the same as above
            // and below it, whatever the height and UI scale.
            const int icon = pixelMetric(PM_LineEditIconSize, option, widget);
            return qMax(0, (widget->height() - icon) / 2 - 3);
        }
        const int base = QProxyStyle::pixelMetric(metric, option, widget);
        if (metric != PM_LineEditIconSize && metric != PM_LineEditIconMargin) return base;
        // Looked up rather than created: a metric query must not make a
        // controller (and read the user's settings) as a side effect.
        const auto* scale = qApp->findChild<UiScaleController*>(QString(), Qt::FindDirectChildrenOnly);
        return scale ? scale->pixels(base) : base;
    }

    QIcon standardIcon(StandardPixmap icon, const QStyleOption* option, const QWidget* widget) const override {
        if (icon == SP_LineEditClearButton) return QIcon(new ClearIconEngine);
        return QProxyStyle::standardIcon(icon, option, widget);
    }
};

// The final stylesheet: tokens resolved, then dimensions scaled.
QString themed(const QString& original) {
    return UiScaleController::instance().styleSheet(resolveTokens(original, accent()));
}

}  // namespace

void defineSettings() {
    core::Settings::instance().define({
        .key = accentColorKey,
        .category = "Appearance",
        .group = "Theme",
        .label = "Accent color",
        .description = "Selections, checked buttons, focus borders and links.",
        .type = core::SettingType::Color,
        .defaultValue = std::string(defaultAccentColor),
    });
}

QColor accent() {
    const QColor color = settingColor(accentColorKey);
    return color.isValid() ? color : QColor(QString::fromLatin1(defaultAccentColor));
}

QColor toastColor(core::ToastKind kind) {
    switch (kind) {
        case core::ToastKind::Success: return QColor(0x4f, 0xd6, 0x71);
        case core::ToastKind::Warning: return QColor(0xff, 0xb3, 0x40);
        case core::ToastKind::Error: return QColor(0xff, 0x5f, 0x56);
        case core::ToastKind::Info: break;
    }
    return accent();
}

QString resolveTokens(const QString& sheet, const QColor& accentColor) {
    QString result = sheet;
    result.replace(QStringLiteral("@accent-rgb"), QStringLiteral("%1, %2, %3")
                                                      .arg(accentColor.red())
                                                      .arg(accentColor.green())
                                                      .arg(accentColor.blue()));
    result.replace(QStringLiteral("@accent-hover"), accentColor.lighter(115).name());
    result.replace(QStringLiteral("@accent"), accentColor.name());
    return result;
}

QIcon appIcon() {
    initThemeResources();
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/usdcc/app/icon-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

void apply(QApplication& app) {
    initThemeResources();
    app.setWindowIcon(appIcon());
    // Linux shells (Wayland always, some X11 ones) match a running window to
    // its launcher entry by this name — which is how they find usdcc.desktop's
    // Icon= and the installed hicolor icons. Harmless elsewhere.
    QGuiApplication::setDesktopFileName(QStringLiteral("usdcc"));
    app.setStyle(new Style);
    defineSettings();
    auto& scale = UiScaleController::instance();
    const auto refresh = [&app]() {
        app.setPalette(darkPalette(accent()));
        app.setStyleSheet(themed(readResource(QStringLiteral(":/usdcc/theme/dark.qss"))));
    };
    // Named so a second apply() (tests) replaces the connections instead of
    // stacking more.
    constexpr auto scaleName = "_usdccThemeScale";
    constexpr auto accentName = "_usdccThemeAccent";
    delete app.findChild<QObject*>(QLatin1String(scaleName), Qt::FindDirectChildrenOnly);
    delete app.findChild<QObject*>(QLatin1String(accentName), Qt::FindDirectChildrenOnly);
    auto* scaleConnection = new QObject(&app);
    scaleConnection->setObjectName(QLatin1String(scaleName));
    QObject::connect(&scale, &UiScaleController::scaleChanged, scaleConnection, [refresh](int) { refresh(); });
    onSettingChanged(&app, accentColorKey, refresh)->setObjectName(QLatin1String(accentName));
    refresh();
}

void applyToDockManager(ads::CDockManager* dockManager) {
    initThemeResources();
    // Capture the original ADS sheet once; reapplying the theme must not grow
    // the stylesheet or multiply already-scaled dimensions.
    constexpr auto key = "_usdccBaseDockStyleSheet";
    if (!dockManager->property(key).isValid()) {
        dockManager->setProperty(key, dockManager->styleSheet());
    }
    const QString original = dockManager->property(key).toString() + QLatin1Char('\n') +
                             readResource(QStringLiteral(":/usdcc/theme/dock.qss"));
    const auto refresh = [dockManager, original]() { dockManager->setStyleSheet(themed(original)); };
    UiScaleController::instance().bind(dockManager, "dockStyle", refresh);
    constexpr auto accentName = "_usdccDockAccent";
    delete dockManager->findChild<QObject*>(QLatin1String(accentName), Qt::FindDirectChildrenOnly);
    onSettingChanged(dockManager, accentColorKey, refresh)->setObjectName(QLatin1String(accentName));
}

}  // namespace usdcc::ui::theme
