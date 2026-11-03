#include "usdcc/usd/usda_syntax.h"

#include <QSet>
#include <QString>

#include <algorithm>

namespace usdcc::usd::usda {

namespace {

template <typename C>
bool isDigit(C c) {
    return c >= '0' && c <= '9';
}
template <typename C>
bool isIdentifierStart(C c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
template <typename C>
bool isIdentifierPart(C c) {
    return isIdentifierStart(c) || isDigit(c) || c == ':' || c == '.';
}

// The characters of a line as plain values ('a', '"', ...) for both the
// UTF-8 scan and QString text: only ASCII is significant in USDA syntax.
template <typename C>
struct Line {
    const C* data;
    int size;
    int at(int i) const { return i < size ? int(data[i]) : 0; }
    bool startsWith(int i, const char* text) const {
        for (int k = 0; text[k]; ++k) {
            if (at(i + k) != text[k]) return false;
        }
        return true;
    }
};

// Where a string closes, from `i` (past its opening quotes); -1 if not on
// this line. A backslash escapes the next character.
template <typename C>
int stringEnd(const Line<C>& line, int i, int quote, bool triple) {
    while (i < line.size) {
        const int c = line.at(i);
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == quote && (!triple || (line.at(i + 1) == quote && line.at(i + 2) == quote))) {
            return i + (triple ? 3 : 1);
        }
        ++i;
    }
    return -1;
}

// Walks a line: calls `span(start, length, kind)` for each token, and leaves
// `*state` as the line ends. With `withSpans` false, only the strings and
// comments are followed, which is all the state needs.
template <typename C, typename SpanFn>
void scan(const Line<C>& line, State* state, bool withSpans, SpanFn span) {
    int i = 0;
    if (*state != Outside) {
        const int quote = *state == InTripleDouble ? '"' : '\'';
        const int end = stringEnd(line, 0, quote, true);
        span(0, end < 0 ? line.size : end, Token::String);
        if (end < 0) return;  // still inside
        *state = Outside;
        i = end;
    }
    while (i < line.size) {
        const int c = line.at(i);
        if (c == '#') {
            span(i, line.size - i, Token::Comment);
            return;
        }
        if (c == '"' || c == '\'') {
            const bool triple = line.at(i + 1) == c && line.at(i + 2) == c;
            const int end = stringEnd(line, i + (triple ? 3 : 1), c, triple);
            if (end < 0) {
                span(i, line.size - i, Token::String);
                if (triple) *state = c == '"' ? InTripleDouble : InTripleSingle;
                return;  // an unclosed single-line string just runs to the end
            }
            span(i, end - i, Token::String);
            i = end;
            continue;
        }
        if (c == '@') {
            const bool triple = line.startsWith(i, "@@@");
            int end = i + (triple ? 3 : 1);
            while (end < line.size && !(triple ? line.startsWith(end, "@@@") : line.at(end) == '@')) ++end;
            end = std::min(line.size, end + (triple ? 3 : 1));
            span(i, end - i, Token::AssetPath);
            i = end;
            continue;
        }
        if (!withSpans) {
            ++i;
            continue;
        }
        if (c == '<') {
            int end = i + 1;
            while (end < line.size && line.at(end) != '>') ++end;
            end = std::min(line.size, end + 1);
            span(i, end - i, Token::Path);
            i = end;
            continue;
        }
        if (isDigit(c) || ((c == '-' || c == '+' || c == '.') && (isDigit(line.at(i + 1)) || (line.at(i + 1) == '.' && isDigit(line.at(i + 2)))))) {
            // Not the end of an identifier ("float3").
            int end = i + 1;
            while (isDigit(line.at(end)) || line.at(end) == '.') ++end;
            if (line.at(end) == 'e' || line.at(end) == 'E') {
                int exponent = end + 1;
                if (line.at(exponent) == '-' || line.at(exponent) == '+') ++exponent;
                if (isDigit(line.at(exponent))) {
                    end = exponent;
                    while (isDigit(line.at(end))) ++end;
                }
            }
            span(i, end - i, Token::Number);
            i = end;
            continue;
        }
        if (isIdentifierStart(c)) {
            int end = i + 1;
            while (isIdentifierPart(line.at(end))) ++end;
            span(i, end - i, Token::Key);  // a placeholder kind, classified by the caller
            i = end;
            continue;
        }
        ++i;
    }
}

const QSet<QString>& keywords() {
    static const QSet<QString> words{
        QStringLiteral("def"),        QStringLiteral("over"),        QStringLiteral("class"),
        QStringLiteral("prepend"),    QStringLiteral("append"),      QStringLiteral("add"),
        QStringLiteral("delete"),     QStringLiteral("reorder"),     QStringLiteral("uniform"),
        QStringLiteral("custom"),     QStringLiteral("varying"),     QStringLiteral("config"),
        QStringLiteral("variantSet"), QStringLiteral("rel"),         QStringLiteral("None"),
        QStringLiteral("true"),       QStringLiteral("false"),       QStringLiteral("inf"),
        QStringLiteral("nan")};
    return words;
}

const QSet<QString>& valueTypes() {
    static const QSet<QString> types = [] {
        QSet<QString> names{QStringLiteral("bool"),     QStringLiteral("uchar"),    QStringLiteral("int"),
                            QStringLiteral("uint"),     QStringLiteral("int64"),    QStringLiteral("uint64"),
                            QStringLiteral("half"),     QStringLiteral("float"),    QStringLiteral("double"),
                            QStringLiteral("timecode"), QStringLiteral("string"),   QStringLiteral("token"),
                            QStringLiteral("asset"),    QStringLiteral("opaque"),   QStringLiteral("pathExpression"),
                            QStringLiteral("dictionary")};
        for (const char* base : {"int", "half", "float", "double"}) {
            for (int n = 2; n <= 4; ++n) names.insert(QStringLiteral("%1%2").arg(QLatin1String(base)).arg(n));
        }
        for (const char* role : {"point3", "normal3", "vector3", "color3", "color4", "texCoord2", "texCoord3"}) {
            for (const char* scalar : {"h", "f", "d"}) names.insert(QLatin1String(role) + QLatin1String(scalar));
        }
        for (const char* m : {"matrix2d", "matrix3d", "matrix4d", "frame4d", "quath", "quatf", "quatd"}) {
            names.insert(QLatin1String(m));
        }
        return names;
    }();
    return types;
}

}  // namespace

std::vector<Span> tokenize(QStringView text, State* state) {
    std::vector<Span> spans;
    // Identifiers come back as Key and are classified here: a keyword, a
    // value type, the prim type after def/over/class, a name before '=', or
    // nothing.
    bool expectPrimType = false;
    scan(Line<char16_t>{reinterpret_cast<const char16_t*>(text.data()), int(text.size())}, state, true,
         [&](int start, int length, Token kind) {
             if (kind != Token::Key) {
                 spans.push_back({start, length, kind});
                 expectPrimType = false;
                 return;
             }
             const QString word = text.mid(start, length).toString();
             if (keywords().contains(word)) {
                 spans.push_back({start, length, Token::Keyword});
                 expectPrimType = word == QLatin1String("def") || word == QLatin1String("over") ||
                                  word == QLatin1String("class");
                 return;
             }
             if (expectPrimType || valueTypes().contains(word)) {
                 spans.push_back({start, length, Token::Type});
                 expectPrimType = false;
                 return;
             }
             expectPrimType = false;
             int next = start + length;
             while (next < text.size() && text[next] == u' ') ++next;
             if (next < text.size() && text[next] == u'=') spans.push_back({start, length, Token::Key});
         });
    return spans;
}

std::vector<std::uint8_t> lineStates(std::string_view text) {
    std::vector<std::uint8_t> states;
    states.reserve(text.size() / 32 + 1);
    State state = Outside;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        states.push_back(state);
        scan(Line<char>{text.data() + start, int(end - start)}, &state, false, [](int, int, Token) {});
        start = end + 1;
    }
    return states;
}

}  // namespace usdcc::usd::usda
