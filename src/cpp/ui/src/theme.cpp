#include "usdcc/ui/theme.h"

#include <DockManager.h>

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>

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
QPalette darkPalette() {
    const QColor panel(0x1c, 0x1c, 0x1e);
    const QColor inset(0x14, 0x14, 0x16);
    const QColor text(0xe8, 0xe8, 0xea);
    const QColor accent(0x3d, 0x8b, 0xfd);
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

}  // namespace

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
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setPalette(darkPalette());
    app.setStyleSheet(readResource(QStringLiteral(":/usdcc/theme/dark.qss")));
}

void applyToDockManager(ads::CDockManager* dockManager) {
    initThemeResources();
    dockManager->setStyleSheet(dockManager->styleSheet() + QLatin1Char('\n') +
                               readResource(QStringLiteral(":/usdcc/theme/dock.qss")));
}

}  // namespace usdcc::ui::theme
