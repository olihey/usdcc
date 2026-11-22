// usdcc.css (the web pages' theme, docs/RENDER_NODE.md §4.5) has the tokens listed at the top
// of dark.qss (the app's): the same surfaces, hairlines, text, radii and type.
#include "test_harness.h"

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include <utility>
#include <vector>

namespace {

QString readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// "--name: value;" in the CSS, without spaces in the value.
QString cssValue(const QString& css, const QString& name) {
    const QRegularExpression pattern(QStringLiteral("%1\\s*:\\s*([^;]+);").arg(QRegularExpression::escape(name)));
    const QRegularExpressionMatch match = pattern.match(css);
    return match.hasMatch() ? match.captured(1).remove(QLatin1Char(' ')) : QString();
}

}  // namespace

USDCC_TEST(theme_css_matches_dark_qss) {
    const QString theme = QStringLiteral(USDCC_TEST_DATA_DIR "/../src/cpp/ui/resources/theme/");
    const QString qss = readFile(theme + QStringLiteral("dark.qss"));
    const QString css = readFile(theme + QStringLiteral("usdcc.css"));
    CHECK(!qss.isEmpty() && !css.isEmpty(), "both files read");
    // The header of dark.qss, where its tokens are listed.
    const QString header = qss.left(qss.indexOf(QStringLiteral("*/")));
    // (CSS property, its value, as dark.qss's header writes it)
    const std::vector<std::pair<QString, std::pair<QString, QString>>> tokens = {
        {QStringLiteral("--surface-bar"), {QStringLiteral("#161618"), QStringLiteral("#161618")}},
        {QStringLiteral("--surface-toolbar"), {QStringLiteral("#1a1a1c"), QStringLiteral("#1a1a1c")}},
        {QStringLiteral("--surface-panel"), {QStringLiteral("#1c1c1e"), QStringLiteral("#1c1c1e")}},
        {QStringLiteral("--inset"), {QStringLiteral("#141416"), QStringLiteral("#141416")}},
        {QStringLiteral("--gap"), {QStringLiteral("#0e0e10"), QStringLiteral("#0e0e10")}},
        {QStringLiteral("--line"), {QStringLiteral("rgba(255,255,255,0.06)"), QStringLiteral("6%")}},
        {QStringLiteral("--line-input"), {QStringLiteral("rgba(255,255,255,0.08)"), QStringLiteral("8%")}},
        {QStringLiteral("--line-popup"), {QStringLiteral("rgba(255,255,255,0.10)"), QStringLiteral("10%")}},
        {QStringLiteral("--text"), {QStringLiteral("#e8e8ea"), QStringLiteral("#e8e8ea")}},
        {QStringLiteral("--text-secondary"), {QStringLiteral("rgba(255,255,255,0.75)"), QStringLiteral("75%")}},
        {QStringLiteral("--text-muted"), {QStringLiteral("rgba(255,255,255,0.50)"), QStringLiteral("50%")}},
        {QStringLiteral("--text-label"), {QStringLiteral("rgba(255,255,255,0.40)"), QStringLiteral("40%")}},
        {QStringLiteral("--accent"), {QStringLiteral("#3d8bfd"), QStringLiteral("#3d8bfd")}},
        {QStringLiteral("--radius"), {QStringLiteral("3px"), QStringLiteral("3px controls")}},
        {QStringLiteral("--radius-group"), {QStringLiteral("4px"), QStringLiteral("4px groups")}},
        {QStringLiteral("--font-size"), {QStringLiteral("11px"), QStringLiteral("11px type")}},
        {QStringLiteral("--control-height"), {QStringLiteral("20px"), QStringLiteral("20px controls")}},
    };
    for (const auto& [property, values] : tokens) {
        const auto& [cssExpected, qssText] = values;
        CHECK(header.contains(qssText), "dark.qss's header still says %s (for %s)", qPrintable(qssText), qPrintable(property));
        CHECK(cssValue(css, property) == cssExpected, "%s is %s in usdcc.css, not %s", qPrintable(property), qPrintable(cssExpected),
              qPrintable(cssValue(css, property)));
    }
    // The type, as QWidget's rule has it.
    CHECK(qss.contains(QStringLiteral("font-family: \"Inter\", \"Segoe UI\"")) &&
              cssValue(css, QStringLiteral("--font")).startsWith(QStringLiteral("\"Inter\",\"SegoeUI\"")),
          "the same font");
}
