#include "usdcc/ui/theme.h"

#include "usdcc/ui/app_settings.h"
#include "usdcc/ui/scale_controller.h"
#include "usdcc/ui/settings.h"

#include <DockManager.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIconEngine>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPalette>
#include <QPixmap>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QSet>
#include <QStyleHints>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Every token: its name in stylesheets and theme files, and its colour in
// usdcc Dark and usdcc Light. In the enum's order.
struct TokenInfo {
    Token token;
    const char* name;
    QRgb dark;
    QRgb light;
};
constexpr TokenInfo kTokens[] = {
    {Token::SurfaceBar, "surface-bar", 0x161618, 0xe4e4e8},
    {Token::SurfaceToolbar, "surface-toolbar", 0x1a1a1c, 0xebebee},
    {Token::SurfacePanel, "surface-panel", 0x1c1c1e, 0xf3f3f5},
    {Token::SurfaceRaised, "surface-raised", 0x232326, 0xfcfcfd},
    {Token::SurfaceHover, "surface-hover", 0x2c2c30, 0xe6e6ea},
    {Token::SurfaceReadOnly, "surface-readonly", 0x222225, 0xececef},
    {Token::Inset, "inset", 0x141416, 0xffffff},
    {Token::Gap, "gap", 0x0e0e10, 0xc9c9ce},
    {Token::BevelLight, "bevel-light", 0x2a2a2e, 0xffffff},
    {Token::BevelMidlight, "bevel-midlight", 0x242428, 0xebebee},
    {Token::BevelMid, "bevel-mid", 0x333337, 0xc8c8cd},
    {Token::Shadow, "shadow", 0x000000, 0xc8c8cd},
    {Token::Text, "text", 0xe8e8ea, 0x1c1c1f},
    {Token::TextStrong, "text-strong", 0xffffff, 0x000000},
    {Token::Ink, "ink", 0xffffff, 0x000000},
    {Token::Overlay, "overlay", 0xffffff, 0x000000},
    {Token::Muted, "muted", 0x8a8d93, 0x63666d},
    {Token::MutedStrong, "muted-strong", 0xa8aab0, 0x4f5258},
    {Token::Icon, "icon", 0xa0a0a4, 0x5c5c62},
    {Token::IconDisabled, "icon-disabled", 0x4a4a4e, 0xb8b8be},
    {Token::IconGrip, "icon-grip", 0x68686e, 0x9a9aa0},
    {Token::Accent, "accent", 0x3d8bfd, 0x2f6fe0},
    {Token::AccentText, "accent-text", 0x3d8bfd, 0x1f5fcf},
    {Token::OnAccent, "on-accent", 0xffffff, 0xffffff},
    {Token::Error, "error", 0xff5f56, 0xc9302c},
    {Token::Warning, "warning", 0xffb340, 0x9a5b00},
    {Token::Attention, "attention", 0xffbd2e, 0x946000},
    {Token::Success, "success", 0x4fd671, 0x1a8a3c},
    {Token::SyntaxKeyword, "syntax-keyword", 0x3d8bfd, 0x0b57d0},
    {Token::SyntaxBuiltin, "syntax-builtin", 0x8ab8ff, 0x3a68ad},
    {Token::SyntaxString, "syntax-string", 0x4fd671, 0x1a7f37},
    {Token::SyntaxNumber, "syntax-number", 0xffbd2e, 0x985600},
    {Token::SyntaxPath, "syntax-path", 0xe5c07b, 0x835500},
    {Token::SyntaxKey, "syntax-key", 0xc8c8cc, 0x3a3a40},
    {Token::PrimInstance, "prim-instance", 0x87cefa, 0x0b6cab},
    {Token::PrimArcs, "prim-arcs", 0xde9e2e, 0x8f5a00},
    {Token::PrimPrototype, "prim-prototype", 0x7688d9, 0x4456b4},
    {Token::GraphGridFine, "graph-grid-fine", 0x242426, 0xe6e6ea},
    {Token::GraphGridCoarse, "graph-grid-coarse", 0x2e2e31, 0xd6d6db},
    {Token::GraphConnection, "graph-connection", 0x828288, 0x85858b},
    {Token::NodeBody, "node-body", 0x2c2c30, 0xfcfcfd},
    {Token::NodePage, "node-page", 0x36363a, 0xe8e8ec},
    {Token::NodeBorder, "node-border", 0x46464a, 0xc0c0c6},
    {Token::NodeBorderHover, "node-border-hover", 0x6e6e74, 0x909096},
    {Token::NodeText, "node-text", 0xe1e1e4, 0x202024},
    {Token::NodeTextMuted, "node-text-muted", 0x96969b, 0x6a6a70},
    {Token::NodePageText, "node-page-text", 0xcdcdd2, 0x44444a},
};
constexpr std::size_t kTokenCount = std::size_t(Token::Count);
static_assert(std::size(kTokens) == kTokenCount);
constexpr bool inOrder() {
    for (std::size_t i = 0; i < kTokenCount; ++i) {
        if (std::size_t(kTokens[i].token) != i) return false;
    }
    return true;
}
static_assert(inOrder());

using Colors = std::array<QColor, kTokenCount>;

Colors baseColors(bool dark) {
    Colors colors;
    for (std::size_t i = 0; i < kTokenCount; ++i) colors[i] = QColor(QRgb(0xff000000u | (dark ? kTokens[i].dark : kTokens[i].light)));
    return colors;
}

struct Theme {
    QString name = QString::fromLatin1(defaultThemeName);
    bool dark = true;
    Colors colors = baseColors(true);
};

// The theme in use: usdcc Dark until apply().
Theme& current() {
    static Theme theme;
    return theme;
}

core::Signal<>& changedSignal() {
    static core::Signal<> signal;
    return signal;
}

// The token called `name`, which must be all of it.
std::optional<std::size_t> tokenIndex(QStringView name) {
    for (std::size_t i = 0; i < kTokenCount; ++i) {
        if (name == QLatin1String(kTokens[i].name)) return i;
    }
    return std::nullopt;
}

bool readTheme(const QString& path, Theme* theme, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!document.isObject()) {
        return fail(parse.error == QJsonParseError::NoError ? QStringLiteral("it isn't a JSON object") : parse.errorString());
    }
    const QJsonObject json = document.object();
    const QString name = json.value(QLatin1String("name")).toString().trimmed();
    if (name.isEmpty()) return fail(QStringLiteral("it has no \"name\""));
    const QString base = json.value(QLatin1String("base")).toString(QStringLiteral("dark"));
    if (base != QLatin1String("dark") && base != QLatin1String("light")) {
        return fail(QStringLiteral("\"base\" is \"%1\", not \"dark\" or \"light\"").arg(base));
    }
    Theme read;
    read.name = name;
    read.dark = base == QLatin1String("dark");
    read.colors = baseColors(read.dark);
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    const QJsonObject colors = json.value(QLatin1String("colors")).toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const auto index = tokenIndex(it.key());
        if (!index) return fail(QStringLiteral("there is no colour called \"%1\"").arg(it.key()));
        const QString value = it.value().toString();
        if (!hex.match(value).hasMatch()) return fail(QStringLiteral("\"%1\" isn't \"#rrggbb\"").arg(it.key()));
        read.colors[*index] = QColor(value);
    }
    // An accent without its text colour: the accent itself, not the base's blue.
    if (colors.contains(QLatin1String("accent")) && !colors.contains(QLatin1String("accent-text"))) {
        read.colors[std::size_t(Token::AccentText)] = read.colors[std::size_t(Token::Accent)];
    }
    *theme = std::move(read);
    return true;
}

// The built-in themes that are files, then the user's.
std::vector<Theme> fileThemes() {
    initThemeResources();
    std::vector<Theme> themes;
    QSet<QString> names{QString::fromLatin1(systemThemeName), QString::fromLatin1(defaultThemeName), QString::fromLatin1(lightThemeName)};
    for (const QString& folder : {QStringLiteral(":/usdcc/themes"), userThemesFolder()}) {
        for (const QFileInfo& file : QDir(folder).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            Theme theme;
            QString error;
            if (!readTheme(file.filePath(), &theme, &error)) {
                qWarning("Theme %s can't be used: %s", qPrintable(file.filePath()), qPrintable(error));
            } else if (names.contains(theme.name)) {
                qWarning("Theme %s can't be used: there is a theme called %s already", qPrintable(file.filePath()), qPrintable(theme.name));
            } else {
                names.insert(theme.name);
                themes.push_back(std::move(theme));
            }
        }
    }
    return themes;
}

Theme builtInTheme(bool dark) {
    Theme theme;
    theme.name = QString::fromLatin1(dark ? defaultThemeName : lightThemeName);
    theme.dark = dark;
    theme.colors = baseColors(dark);
    return theme;
}

Theme findTheme(const QString& name) {
    if (name == QLatin1String(lightThemeName)) return builtInTheme(false);
    if (name == QLatin1String(systemThemeName)) {
        return builtInTheme(QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light);
    }
    if (name != QLatin1String(defaultThemeName)) {
        for (Theme& theme : fileThemes()) {
            if (theme.name == name) return std::move(theme);
        }
    }
    return builtInTheme(true);
}

double luminance(const QColor& color) {
    const auto linear = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

// Whether the accent is the user's own rather than the theme's.
bool ownAccent() { return accent() != current().colors[std::size_t(Token::Accent)]; }

// Dark text needs to be a little stronger than light text at the same opacity.
// Faint on purpose (disabled) stays faint.
double inkOpacity(double opacity) {
    return current().dark || opacity < 0.34 ? opacity : opacity + (1.0 - opacity) * 0.25;
}

// Whether any icon looks different than in its file.
bool tintsIcons() {
    const auto differs = [](Token token) { return color(token).rgb() != (0xff000000u | kTokens[std::size_t(token)].dark); };
    return differs(Token::Icon) || differs(Token::IconDisabled) || differs(Token::IconGrip) || differs(Token::OnAccent);
}

QString tintKey() {
    return color(Token::Icon).name().mid(1) + color(Token::IconDisabled).name().mid(1) + color(Token::IconGrip).name().mid(1) +
           color(Token::OnAccent).name().mid(1);
}

QString doubleSize(const QString& path) {
    QString at2 = path;
    return at2.insert(path.lastIndexOf(QLatin1Char('.')), QLatin1String("@2x"));
}

// The stylesheets' images, recoloured: a stylesheet can only name a file, so
// they are written to a folder of their own, one per set of colours (Qt caches
// images by name), reached as "usdcc-tinted:".
QString withTintedImages(const QString& sheet) {
    if (!tintsIcons()) return sheet;
    static QTemporaryDir folder;
    if (!folder.isValid()) return sheet;
    const QString prefix = QStringLiteral("usdcc-tinted");
    if (QDir::searchPaths(prefix).isEmpty()) QDir::addSearchPath(prefix, folder.path());
    const QString key = tintKey();
    QDir(folder.path()).mkpath(key);
    static const QRegularExpression urls(QStringLiteral("url\\(:/usdcc/theme/([^)]+)\\)"));
    QString result = sheet;
    QSet<QString> done;
    for (auto it = urls.globalMatch(sheet); it.hasNext();) {
        const QString name = it.next().captured(1);
        if (done.contains(name)) continue;
        done.insert(name);
        const QString source = QStringLiteral(":/usdcc/theme/") + name;
        const QString target = folder.filePath(key + QLatin1Char('/') + name);
        if (!QFile::exists(target)) {
            if (!tintedImage(source).save(target)) continue;
            if (QFile::exists(doubleSize(source))) tintedImage(doubleSize(source)).save(doubleSize(target));
        }
        result.replace(QStringLiteral("url(%1)").arg(source), QStringLiteral("url(%1:%2/%3)").arg(prefix, key, name));
    }
    return result;
}

// The theme's line icons, in the colours of the moment.
class ThemeIconEngine : public QIconEngine {
public:
    ThemeIconEngine(QString path, QString onPath) : m_path(std::move(path)), m_onPath(std::move(onPath)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        tinted().paint(painter, rect, Qt::AlignCenter, mode, state);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        return tinted().pixmap(size, scale, mode, state);
    }
    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return tinted().actualSize(size, mode, state); }
    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override { return tinted().availableSizes(mode, state); }
    bool isNull() override { return !QFile::exists(m_path); }
    QIconEngine* clone() const override { return new ThemeIconEngine(m_path, m_onPath); }
    QString key() const override { return QStringLiteral("usdcc-theme-icon"); }

private:
    const QIcon& tinted() {
        const QString key = tintKey();
        if (key != m_key) {
            m_key = key;
            m_icon = QIcon();
            add(m_path, QIcon::Off);
            if (!m_onPath.isEmpty()) add(m_onPath, QIcon::On);
        }
        return m_icon;
    }

    void add(const QString& path, QIcon::State state) {
        m_icon.addPixmap(pixmap(path, state), QIcon::Normal, state);
        const QString at2 = doubleSize(path);
        if (!QFile::exists(at2)) return;
        QPixmap large = pixmap(at2, state);
        large.setDevicePixelRatio(2.0);
        m_icon.addPixmap(large, QIcon::Normal, state);
    }

    // A checked button is filled with the accent.
    static QPixmap pixmap(const QString& path, QIcon::State state) {
        QPixmap result = QPixmap::fromImage(tintedImage(path));
        if (state == QIcon::On) {
            QPainter painter(&result);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(result.rect(), color(Token::OnAccent));
        }
        return result;
    }

    QString m_path;
    QString m_onPath;
    QString m_key;
    QIcon m_icon;
};

// The palette is what widgets the stylesheet doesn't cover (and Fusion's own
// drawing) fall back to.
QPalette themePalette() {
    const QColor text = color(Token::Text);
    const QColor disabledText = color(Token::Ink, 80);
    const QColor accentText = color(Token::AccentText);

    QPalette p;
    p.setColor(QPalette::Window, color(Token::SurfacePanel));
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, color(Token::Inset));
    p.setColor(QPalette::AlternateBase, color(Token::SurfacePanel));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, color(Token::SurfaceRaised));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, color(Token::TextStrong));
    p.setColor(QPalette::ToolTipBase, color(Token::SurfaceRaised));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, color(Token::Ink, 100));
    p.setColor(QPalette::Highlight, accent());
    p.setColor(QPalette::HighlightedText, color(Token::OnAccent));
    p.setColor(QPalette::Link, accentText);
    p.setColor(QPalette::LinkVisited, isDark() ? accentText.lighter(130) : accentText.darker(130));

    // ADS's own rules reference these (palette(light) / palette(dark)).
    p.setColor(QPalette::Light, color(Token::BevelLight));
    p.setColor(QPalette::Midlight, color(Token::BevelMidlight));
    p.setColor(QPalette::Mid, color(Token::BevelMid));
    p.setColor(QPalette::Dark, color(Token::Gap));
    p.setColor(QPalette::Shadow, color(Token::Shadow));

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
        painter->fillPath(shape, color(Token::Ink, alpha));
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

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override {
        // A tree row's indent: the stylesheet paints it with the row's
        // ::item:hover or ::item:selected background, a rounded block of its
        // own in front of the item's. QTreeView draws the branches over all of
        // the indent afterwards, so the view's background, laid here first,
        // leaves the item's highlight alone in the row (and the arrows on top).
        if (element == PE_IndicatorBranch) {
            const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
            const bool alternate = item && item->features.testFlag(QStyleOptionViewItem::Alternate);
            painter->fillRect(option->rect, option->palette.brush(alternate ? QPalette::AlternateBase : QPalette::Base));
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

// The final stylesheet: tokens resolved, then dimensions scaled.
QString themed(const QString& original) {
    return UiScaleController::instance().styleSheet(withTintedImages(resolveTokens(original, accent())));
}

}  // namespace

QString tokenName(Token token) { return QString::fromLatin1(kTokens[std::size_t(token)].name); }

QColor color(Token token, int alpha) {
    const Colors& colors = current().colors;
    QColor result = colors[std::size_t(token)];
    switch (token) {
        case Token::Accent: result = accent(); break;
        case Token::AccentText:
            // Of the theme's accent only: another one is its own text colour.
            if (ownAccent()) result = isDark() ? accent() : accent().darker(115);
            break;
        case Token::OnAccent:
            if (ownAccent()) result = luminance(accent()) > 0.45 ? QColor(0x14, 0x14, 0x16) : QColor(Qt::white);
            break;
        case Token::Ink:
            if (alpha < 255) alpha = qRound(inkOpacity(alpha / 255.0) * 255.0);
            break;
        default: break;
    }
    result.setAlpha(alpha);
    return result;
}

QStringList themeNames() {
    QStringList names{QString::fromLatin1(systemThemeName), QString::fromLatin1(defaultThemeName), QString::fromLatin1(lightThemeName)};
    for (const Theme& theme : fileThemes()) names << theme.name;
    return names;
}

QString userThemesFolder() { return QDir(userDataFolder()).filePath(QStringLiteral("themes")); }

QString currentThemeName() { return current().name; }

bool isDark() { return current().dark; }

bool readThemeFile(const QString& path, QString* name, QString* error) {
    Theme theme;
    if (!readTheme(path, &theme, error)) return false;
    if (name) *name = theme.name;
    return true;
}

QObject* onChanged(QObject* owner, std::function<void()> changed) {
    // Deleted (and so disconnected) with its owner.
    class ThemeConnection : public QObject {
    public:
        ThemeConnection(QObject* owner, core::Signal<>::Connection connection) : QObject(owner), m_connection(std::move(connection)) {}

    private:
        core::Signal<>::Connection m_connection;
    };
    return new ThemeConnection(owner, changedSignal().connect(std::move(changed)));
}

QIcon icon(const QString& path) { return icon(path, QString()); }

QIcon icon(const QString& path, const QString& onPath) {
    // Only the theme's own: a plugin's icon has the colours it has.
    if (!path.startsWith(QLatin1String(":/usdcc/theme/"))) {
        QIcon plain(path);
        if (!onPath.isEmpty()) plain.addFile(onPath, QSize(), QIcon::Normal, QIcon::On);
        return plain;
    }
    initThemeResources();
    return QIcon(new ThemeIconEngine(path, onPath));
}

QImage tintedImage(const QString& path) {
    QImage image = QImage(path).convertToFormat(QImage::Format_ARGB32);
    // White on the close button's red, which no theme changes.
    if (path.contains(QLatin1String("win-close-hover"))) return image;
    // tools/gen_theme_icons.py's colours: LIGHT, DIM, GRIP and WHITE.
    const std::pair<QRgb, Token> tints[] = {
        {0xa0a0a4, Token::Icon}, {0x4a4a4e, Token::IconDisabled}, {0x68686e, Token::IconGrip}, {0xffffff, Token::OnAccent}};
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            for (const auto& [from, token] : tints) {
                if ((line[x] & 0xffffffu) != from) continue;
                line[x] = (line[x] & 0xff000000u) | (color(token).rgb() & 0xffffffu);
                break;
            }
        }
    }
    return image;
}

void defineSettings() {
    auto& settings = core::Settings::instance();
    std::vector<std::string> names;
    for (const QString& name : themeNames()) names.push_back(name.toStdString());
    settings.define({
        .key = themeKey,
        .category = "Appearance",
        .group = "Theme",
        .label = "Theme",
        .description = "The colors of the whole application. System is usdcc Dark or usdcc Light, as the system's own setting is.",
        .type = core::SettingType::Choice,
        .defaultValue = std::string(defaultThemeName),
        .choices = std::move(names),
    });
    // The default accent is the theme's: an accent nobody chose goes with it.
    const bool chosen = settings.contains(accentColorKey) && !settings.isDefault(accentColorKey);
    settings.define({
        .key = accentColorKey,
        .category = "Appearance",
        .group = "Theme",
        .label = "Accent color",
        .description = "Selections, checked buttons, focus borders and links. The default is the theme's own.",
        .type = core::SettingType::Color,
        .defaultValue = current().colors[std::size_t(Token::Accent)].name().toStdString(),
    });
    if (!chosen && !settings.isDefault(accentColorKey)) settings.reset(accentColorKey);
}

QColor accent() {
    const QColor own = settingColor(accentColorKey);
    return own.isValid() ? own : current().colors[std::size_t(Token::Accent)];
}

QColor toastColor(core::ToastKind kind) {
    switch (kind) {
        case core::ToastKind::Success: return color(Token::Success);
        case core::ToastKind::Warning: return color(Token::Warning);
        case core::ToastKind::Error: return color(Token::Error);
        case core::ToastKind::Info: break;
    }
    return accent();
}

QString resolveTokens(const QString& sheet, const QColor& accentColor) {
    static const QRegularExpression tokens(QStringLiteral("@([a-z]+(?:-[a-z]+)*)(?:, *(\\d+)%)?"));
    QString result;
    qsizetype offset = 0;
    for (auto it = tokens.globalMatch(sheet); it.hasNext();) {
        const auto match = it.next();
        result += sheet.mid(offset, match.capturedStart() - offset);
        offset = match.capturedEnd();
        QString name = match.captured(1);
        const bool rgb = name.endsWith(QLatin1String("-rgb"));
        if (rgb) name.chop(4);
        QColor value;
        if (name == QLatin1String("accent")) {
            value = accentColor;
        } else if (name == QLatin1String("accent-hover")) {
            value = accentColor.lighter(115);
        } else if (const auto index = tokenIndex(name)) {
            value = color(Token(*index));
        } else {
            result += match.captured();  // not a token
            continue;
        }
        result += rgb ? QStringLiteral("%1, %2, %3").arg(value.red()).arg(value.green()).arg(value.blue()) : value.name();
        if (match.captured(2).isEmpty()) {
            result += match.captured().mid(1 + match.capturedLength(1));
        } else {
            const double opacity = match.captured(2).toInt() / 100.0;
            result += QStringLiteral(", %1%").arg(qRound((name == QLatin1String("ink") ? inkOpacity(opacity) : opacity) * 100.0));
        }
    }
    return result + sheet.mid(offset);
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
    auto& scale = UiScaleController::instance();
    const auto refresh = [&app]() {
        app.setPalette(themePalette());
        app.setStyleSheet(themed(readResource(QStringLiteral(":/usdcc/theme/app.qss"))));
        changedSignal()();
    };
    // Makes the theme the setting names the current one. Its accent becomes the
    // accent setting's default, which says that it changed: no refresh for that.
    const auto switching = std::make_shared<bool>(false);
    const auto load = [switching]() {
        *switching = true;
        const QString name = QString::fromStdString(core::Settings::instance().get<std::string>(themeKey));
        auto* hints = QGuiApplication::styleHints();
        const bool system = name == QLatin1String(systemThemeName);
        // What the system says, asked before it is told anything.
        if (system) hints->unsetColorScheme();
        current() = findTheme(name);
        // The title bars the system draws (dialogs).
        if (!system) hints->setColorScheme(current().dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        defineSettings();
        *switching = false;
    };
    defineSettings();
    load();
    // Named so a second apply() (tests) replaces the connections instead of
    // stacking more.
    constexpr auto connectionsName = "_usdccThemeConnections";
    delete app.findChild<QObject*>(QLatin1String(connectionsName), Qt::FindDirectChildrenOnly);
    auto* connections = new QObject(&app);
    connections->setObjectName(QLatin1String(connectionsName));
    QObject::connect(&scale, &UiScaleController::scaleChanged, connections, [refresh](int) { refresh(); });
    onSettingChanged(connections, accentColorKey, [refresh, switching]() {
        if (!*switching) refresh();
    });
    onSettingChanged(connections, themeKey, [load, refresh, switching]() {
        if (*switching) return;
        load();
        refresh();
    });
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, connections, [load, refresh, switching]() {
        if (*switching || core::Settings::instance().get<std::string>(themeKey) != systemThemeName) return;
        load();
        refresh();
    });
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
    constexpr auto themeName = "_usdccDockTheme";
    delete dockManager->findChild<QObject*>(QLatin1String(themeName), Qt::FindDirectChildrenOnly);
    onChanged(dockManager, refresh)->setObjectName(QLatin1String(themeName));
}

}  // namespace usdcc::ui::theme
