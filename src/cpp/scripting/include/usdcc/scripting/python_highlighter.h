#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace usdcc::scripting {

// Python syntax highlighting for the Script Editor panel's editor
// (docs/SCRIPTING.md §7), in the dark theme's colors: keywords, builtins
// (and self/cls), numbers, strings — including triple-quoted ones across
// lines — comments, decorators, and the names after def/class.
class PythonHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    enum class Kind { Keyword, Builtin, Number, String, Comment, Decorator, Definition };

    explicit PythonHighlighter(QTextDocument* document);

    // The format used for `kind` (exposed for tests).
    QTextCharFormat format(Kind kind) const;

protected:
    void highlightBlock(const QString& text) override;
};

}  // namespace usdcc::scripting
