#include "usdcc/scripting/python_highlighter.h"

#include <QColor>
#include <QRegularExpression>

namespace usdcc::scripting {

namespace {
// Block states: inside a triple-quoted string that continues on the next line.
enum BlockState { Normal = -1, InTripleSingle = 1, InTripleDouble = 2 };

QTextCharFormat makeFormat(const QColor& color, bool italic = false) {
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontItalic(italic);
    return format;
}

const QRegularExpression& keywords() {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(False|None|True|and|as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|"
        "from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield)\\b"));
    return pattern;
}

const QRegularExpression& builtins() {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(self|cls|print|len|range|str|int|float|bool|list|dict|set|tuple|bytes|object|type|isinstance|"
        "issubclass|super|enumerate|zip|map|filter|sorted|reversed|min|max|sum|abs|round|any|all|open|repr|"
        "getattr|setattr|hasattr|iter|next|id|hash|vars|dir|input|format|Exception|ValueError|TypeError|"
        "KeyError|IndexError|RuntimeError|AttributeError|NotImplementedError|StopIteration)\\b"));
    return pattern;
}

const QRegularExpression& numbers() {
    static const QRegularExpression pattern(
        QStringLiteral("\\b(0[xX][0-9a-fA-F_]+|0[bB][01_]+|[0-9][0-9_]*(\\.[0-9_]*)?([eE][+-]?[0-9]+)?[jJ]?)\\b"));
    return pattern;
}

const QRegularExpression& decorators() {
    static const QRegularExpression pattern(QStringLiteral("^\\s*@[\\w.]+"));
    return pattern;
}

const QRegularExpression& definitions() {
    static const QRegularExpression pattern(QStringLiteral("\\b(def|class)\\s+(\\w+)"));
    return pattern;
}
}  // namespace

PythonHighlighter::PythonHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

QTextCharFormat PythonHighlighter::format(Kind kind) const {
    // The dark theme's tokens (resources/theme/dark.qss): accent blue for
    // keywords, its lighter tint for builtins, the axis green for strings, the
    // log's amber for numbers and decorators, muted text for comments.
    switch (kind) {
        case Kind::Keyword: return makeFormat(QColor(0x3d, 0x8b, 0xfd));
        case Kind::Builtin: return makeFormat(QColor(0x8a, 0xb8, 0xff));
        case Kind::Number: return makeFormat(QColor(0xff, 0xbd, 0x2e));
        case Kind::String: return makeFormat(QColor(0x4f, 0xd6, 0x71));
        case Kind::Comment: return makeFormat(QColor(255, 255, 255, 102), true);
        case Kind::Decorator: return makeFormat(QColor(0xff, 0xbd, 0x2e));
        case Kind::Definition: {
            QTextCharFormat definition = makeFormat(QColor(0xe8, 0xe8, 0xea));
            definition.setFontWeight(QFont::Bold);
            return definition;
        }
    }
    return {};
}

void PythonHighlighter::highlightBlock(const QString& text) {
    auto applyAll = [&](const QRegularExpression& pattern, Kind kind, int group = 0) {
        for (auto it = pattern.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            setFormat(static_cast<int>(match.capturedStart(group)), static_cast<int>(match.capturedLength(group)),
                      format(kind));
        }
    };
    applyAll(numbers(), Kind::Number);
    applyAll(builtins(), Kind::Builtin);
    applyAll(keywords(), Kind::Keyword);
    applyAll(definitions(), Kind::Definition, 2);
    applyAll(decorators(), Kind::Decorator);

    // Strings and comments last, so they win over everything above: a scan
    // that knows a '#' inside a string isn't a comment, and carries a
    // triple-quoted string over to the next line in the block state.
    setCurrentBlockState(Normal);
    const int length = static_cast<int>(text.size());
    int i = 0;
    int stringStart = 0;
    QString quote;  // the open string's delimiter: ', ", ''' or """
    if (previousBlockState() == InTripleSingle) quote = QStringLiteral("'''");
    if (previousBlockState() == InTripleDouble) quote = QStringLiteral("\"\"\"");

    while (i < length) {
        if (!quote.isEmpty()) {
            if (text.at(i) == u'\\') {
                i += 2;
                continue;
            }
            if (text.mid(i, quote.size()) == quote) {
                i += static_cast<int>(quote.size());
                setFormat(stringStart, i - stringStart, format(Kind::String));
                quote.clear();
                continue;
            }
            ++i;
            continue;
        }
        const QChar c = text.at(i);
        if (c == u'#') {
            setFormat(i, length - i, format(Kind::Comment));
            return;
        }
        if (c == u'\'' || c == u'"') {
            // Include a prefix such as r, b, f or rb in the string's color.
            stringStart = i;
            while (stringStart > 0 && QStringLiteral("rRbBfFuU").contains(text.at(stringStart - 1)) &&
                   (stringStart - 1 == 0 || !text.at(stringStart - 2).isLetterOrNumber())) {
                --stringStart;
            }
            const QString triple(3, c);
            quote = text.mid(i, 3) == triple ? triple : QString(c);
            i += static_cast<int>(quote.size());
            continue;
        }
        ++i;
    }

    if (!quote.isEmpty()) {
        setFormat(stringStart, length - stringStart, format(Kind::String));
        if (quote.size() == 3) setCurrentBlockState(quote.at(0) == u'\'' ? InTripleSingle : InTripleDouble);
        // A single-quoted string ends at the line's end either way.
    }
}

}  // namespace usdcc::scripting
