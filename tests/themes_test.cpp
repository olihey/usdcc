// The themes (docs/THEMES.md): the setting, the theme files, the colours and what follows them.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <string>

namespace theme = usdcc::ui::theme;
using theme::Token;
using usdcc::core::Settings;

namespace {

QApplication& themedApplication() { return *static_cast<QApplication*>(QApplication::instance()); }

double luminance(const QColor& c) {
    const auto linear = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
}

double contrast(const QColor& a, const QColor& b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

QColor over(const QColor& top, const QColor& bottom) {
    const float a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + bottom.redF() * (1 - a), top.greenF() * a + bottom.greenF() * (1 - a),
                            top.blueF() * a + bottom.blueF() * (1 - a));
}

// The one colour of a line icon.
QColor iconColor(const QIcon& icon) {
    const QImage image = icon.pixmap(QSize(16, 16), 1.0).toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() == 255) return image.pixelColor(x, y);
        }
    }
    return {};
}

// The theme applied, and usdcc Dark again afterwards.
struct Applied {
    TestSettings settings;
    QTemporaryDir temp;
    usdcc::ui::UiScaleController scale{themedApplication(), temp.filePath(QStringLiteral("ui.ini"))};
    Applied() { theme::apply(themedApplication()); }
    ~Applied() { use(theme::defaultThemeName); }
    static bool use(const std::string& name) { return Settings::instance().setValue(theme::themeKey, name); }
};

}  // namespace

USDCC_TEST(themes_switch) {
    Applied applied;
    auto& settings = Settings::instance();
    CHECK(theme::currentThemeName() == QLatin1String("usdcc Dark") && theme::isDark(), "usdcc Dark is the default");
    CHECK(theme::color(Token::SurfacePanel) == QColor(0x1c, 0x1c, 0x1e), "with the panels' colour it always had");
    CHECK(theme::color(Token::Ink, 102) == QColor(255, 255, 255, 102), "and its text's");
    const QIcon trash = theme::icon(QStringLiteral(":/usdcc/theme/trash.png"));
    CHECK(iconColor(trash) == QColor(0xa0, 0xa0, 0xa4), "its icons' is %s", qPrintable(iconColor(trash).name()));
    const QString darkSheet = themedApplication().styleSheet();
    CHECK(!darkSheet.contains(QStringLiteral("usdcc-tinted:")), "which the stylesheet's images have already");

    int changes = 0;
    QObject owner;
    theme::onChanged(&owner, [&changes]() { ++changes; });
    CHECK(Applied::use("usdcc Light"), "usdcc Light is a choice");
    CHECK(changes == 1, "one change is said once (%d)", changes);
    CHECK(!theme::isDark() && themedApplication().palette().color(QPalette::Window) == QColor(0xf3, 0xf3, 0xf5), "the palette is light");
    CHECK(themedApplication().styleSheet().contains(QStringLiteral("#f3f3f5")), "so is the stylesheet");
    CHECK(iconColor(trash) == theme::color(Token::Icon), "an icon made before draws itself anew: %s", qPrintable(iconColor(trash).name()));
    CHECK(themedApplication().styleSheet().contains(QStringLiteral("usdcc-tinted:")), "the stylesheet's images are recoloured");
    const QRegularExpression urls(QStringLiteral("url\\((usdcc-tinted:[^)]+)\\)"));
    const QString lightSheet = themedApplication().styleSheet();
    for (auto it = urls.globalMatch(lightSheet); it.hasNext();) {
        const QString file = it.next().captured(1);
        CHECK(!QImage(file).isNull(), "%s is an image", qPrintable(file));
    }
    CHECK(iconColor(theme::icon(QStringLiteral(":/usdcc/theme/win-close-hover.png"))) == QColor(Qt::white), "the close button's stays white");

    CHECK(theme::accent() == QColor(0x2f, 0x6f, 0xe0) && settings.isDefault(theme::accentColorKey), "the accent is the theme's");
    settings.setValue(theme::accentColorKey, std::string("#e5484d"));
    CHECK(changes == 2, "the accent's change is said (%d)", changes);
    Applied::use("usdcc Dark");
    CHECK(theme::accent() == QColor(0xe5, 0x48, 0x4d), "one's own accent stays with any theme");
    settings.reset(theme::accentColorKey);
    CHECK(theme::accent() == QColor(0x3d, 0x8b, 0xfd), "and resets to the theme's");
    settings.setValue(theme::accentColorKey, std::string("#ffe94a"));
    CHECK(theme::color(Token::OnAccent).lightness() < 60, "text on a bright accent is dark");
    settings.reset(theme::accentColorKey);
    CHECK(themedApplication().styleSheet() == darkSheet, "back to the same stylesheet");
}

USDCC_TEST(themes_all_resolve_and_read) {
    Applied applied;
    const QStringList names = theme::themeNames();
    CHECK(names.size() >= 7 && names.first() == QLatin1String("System"), "System, two built in and four files: %s", qPrintable(names.join(u',')));
    for (const QString& name : names) {
        CHECK(Applied::use(name.toStdString()), "%s can be set", qPrintable(name));
        if (name == QLatin1String("System")) {
            CHECK(theme::currentThemeName().startsWith(QLatin1String("usdcc ")), "System is one of the two: %s",
                  qPrintable(theme::currentThemeName()));
            continue;
        }
        CHECK(theme::currentThemeName() == name, "%s is in use", qPrintable(name));
        QString sheet = themedApplication().styleSheet();
        sheet.remove(QRegularExpression(QStringLiteral("/\\*[\\s\\S]*?\\*/")));
        CHECK(!sheet.contains(QLatin1Char('@')), "%s: no token is left in the stylesheet", qPrintable(name));
        const auto readable = [&name](Token front, Token back, double least) {
            const double ratio = contrast(theme::color(front), theme::color(back));
            CHECK(ratio >= least, "%s: %s on %s is %.2f, at least %.1f", qPrintable(name), qPrintable(theme::tokenName(front)),
                  qPrintable(theme::tokenName(back)), ratio, least);
        };
        readable(Token::Text, Token::SurfacePanel, 4.5);
        readable(Token::Text, Token::Inset, 4.5);
        readable(Token::Text, Token::SurfaceRaised, 4.5);
        readable(Token::OnAccent, Token::Accent, 3.0);
        readable(Token::AccentText, Token::SurfacePanel, 3.0);
        readable(Token::NodeText, Token::NodeBody, 4.5);
        for (Token token : {Token::Icon, Token::Muted, Token::Error, Token::Warning, Token::Attention, Token::SyntaxKeyword,
                            Token::SyntaxString, Token::SyntaxNumber, Token::SyntaxPath, Token::PrimInstance, Token::PrimArcs,
                            Token::PrimPrototype}) {
            readable(token, Token::SurfacePanel, 3.0);
        }
        const QColor panel = theme::color(Token::SurfacePanel);
        const double secondary = contrast(over(theme::color(Token::Ink, 191), panel), panel);
        CHECK(secondary >= 4.5, "%s: secondary text is %.2f", qPrintable(name), secondary);
    }
}

USDCC_TEST(themes_files) {
    QTemporaryDir temp;
    const auto read = [&temp](const QByteArray& text, QString* error) {
        const QString path = temp.filePath(QStringLiteral("theme.json"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text);
        file.close();
        QString name;
        return theme::readThemeFile(path, &name, error) ? name : QString();
    };
    QString error;
    CHECK(read(R"({"name": "Mine", "base": "light", "colors": {"accent": "#AA0011"}})", &error) == QLatin1String("Mine"), "a theme: %s",
          qPrintable(error));
    CHECK(read(R"({"name": "Bare"})", &error) == QLatin1String("Bare"), "everything but the name has a default");
    for (const char* bad : {"", "[]", "{}", R"({"name": "X", "base": "blue"})", R"({"name": "X", "colors": {"surface": "#000000"}})",
                            R"({"name": "X", "colors": {"accent": "red"}})", R"({"name": "X", "colors": {"accent": 3}})"}) {
        error.clear();
        CHECK(read(bad, &error).isEmpty() && !error.isEmpty(), "not a theme: %s (%s)", bad, qPrintable(error));
    }

    Applied applied;
    const QString folder = theme::userThemesFolder();
    QDir().mkpath(folder);
    const QString path = QDir(folder).filePath(QStringLiteral("usdcc-test-theme.json"));
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly), "%s can be written", qPrintable(path));
        file.write(R"({"name": "Test Theme", "colors": {"surface-panel": "#102030", "accent": "#00ff00"}})");
    }
    theme::defineSettings();
    CHECK(Applied::use("Test Theme"), "the user's theme is a choice");
    CHECK(theme::color(Token::SurfacePanel) == QColor(0x10, 0x20, 0x30) && theme::color(Token::Text) == QColor(0xe8, 0xe8, 0xea),
          "its colours, and the base's for the rest");
    CHECK(theme::color(Token::AccentText) == QColor(0, 255, 0), "text in the accent is its accent");
    Applied::use(theme::defaultThemeName);
    QFile::remove(path);
    theme::defineSettings();
    CHECK(!theme::themeNames().contains(QStringLiteral("Test Theme")), "gone with its file");
}
