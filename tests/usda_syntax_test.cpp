// The USDA tokenizer (docs/USDA_VIEW.md §3): each token kind, strings that
// hide other syntax, multi-line strings across lines, and lineStates(), which
// must agree with tokenize() about where they are.
#include "test_harness.h"

#include "usdcc/usd/usda_syntax.h"

#include <QString>

#include <string>

namespace usda = usdcc::usd::usda;

namespace {
// "kind:text" for each span, to compare at a glance.
std::string describe(const QString& line, usda::State state = usda::Outside, usda::State* end = nullptr) {
    std::string out;
    for (const usda::Span& span : usda::tokenize(line, &state)) {
        static const char* names[] = {"kw", "type", "str", "asset", "path", "num", "key", "comment"};
        if (!out.empty()) out += ' ';
        out += names[int(span.kind)];
        out += ':';
        out += line.mid(span.start, span.length).toStdString();
    }
    if (end) *end = state;
    return out;
}
}  // namespace

USDCC_TEST(usda_tokens) {
    CHECK(describe(QStringLiteral("def Xform \"World\" (")) == "kw:def type:Xform str:\"World\"",
          "a prim: [%s]", describe(QStringLiteral("def Xform \"World\" (")).c_str());
    CHECK(describe(QStringLiteral("    prepend references = @./chair.usda@</Chair>")) ==
              "kw:prepend key:references asset:@./chair.usda@ path:</Chair>",
          "a reference: [%s]", describe(QStringLiteral("    prepend references = @./chair.usda@</Chair>")).c_str());
    CHECK(describe(QStringLiteral("    float3[] extent = [(-1, -2.5, 3e-2)]")) ==
              "type:float3 key:extent num:-1 num:-2.5 num:3e-2",
          "types and numbers: [%s]", describe(QStringLiteral("    float3[] extent = [(-1, -2.5, 3e-2)]")).c_str());
    CHECK(describe(QStringLiteral("    uniform token purpose = \"render\"  # a comment")) ==
              "kw:uniform type:token key:purpose str:\"render\" comment:# a comment",
          "a comment after a value: [%s]",
          describe(QStringLiteral("    uniform token purpose = \"render\"  # a comment")).c_str());
    CHECK(describe(QStringLiteral("    string s = \"not # a comment, \\\" nor @this@\"")) ==
              "type:string key:s str:\"not # a comment, \\\" nor @this@\"",
          "a string hides other syntax, escapes included");
    CHECK(describe(QStringLiteral("    asset tex = @@@C:/odd@name.png@@@")) == "type:asset key:tex asset:@@@C:/odd@name.png@@@",
          "a triple-@ asset path");
    CHECK(describe(QStringLiteral("    double xformOp:rotateX.timeSamples = {")) == "type:double key:xformOp:rotateX.timeSamples",
          "a namespaced name");

    // A doc string across three lines.
    usda::State state = usda::Outside;
    CHECK(describe(QStringLiteral("    doc = \"\"\"First"), usda::Outside, &state) == "key:doc str:\"\"\"First" &&
              state == usda::InTripleDouble,
          "opens a multi-line string");
    CHECK(describe(QStringLiteral("def \"not a prim\""), state, &state) == "str:def \"not a prim\"" &&
              state == usda::InTripleDouble,
          "inside it, everything is string");
    CHECK(describe(QStringLiteral("end\"\"\" (kind = 1)"), state, &state) == "str:end\"\"\" key:kind num:1" &&
              state == usda::Outside,
          "and it closes: [%s]", describe(QStringLiteral("end\"\"\" (kind = 1)"), usda::InTripleDouble).c_str());
}

USDCC_TEST(usda_line_states) {
    const std::string text =
        "#usda 1.0\n"
        "(\n"
        "    doc = \"\"\"Line one\n"
        "# not a comment\n"
        "\"\"\"\n"
        "    comment = \"a \\\"\"\"\" quote\"\n"  // an escaped quote, then an empty "" and a string
        "    # a comment \"\"\" with quotes\n"
        "    customData = { string x = '''multi\n"
        "line''' }\n"
        ")\n";
    const std::vector<std::uint8_t> states = usda::lineStates(text);
    CHECK(states.size() == 11, "a state for each line (and the empty last one): [%zu]", states.size());
    if (states.size() != 11) return;
    CHECK(states[0] == usda::Outside && states[2] == usda::Outside, "outside until the doc string");
    CHECK(states[3] == usda::InTripleDouble && states[4] == usda::InTripleDouble, "inside it for two lines");
    CHECK(states[5] == usda::Outside && states[6] == usda::Outside && states[7] == usda::Outside,
          "out after it; quotes in a string or a comment don't open one");
    CHECK(states[8] == usda::InTripleSingle && states[9] == usda::Outside, "a ''' string for one line");

    // tokenize() agrees, line by line.
    usda::State state = usda::Outside;
    std::size_t start = 0;
    for (std::size_t line = 0; line + 1 < states.size(); ++line) {
        const std::size_t end = text.find('\n', start);
        CHECK(state == states[line], "line %zu starts in the same state", line + 1);
        usda::tokenize(QString::fromStdString(text.substr(start, end - start)), &state);
        start = end + 1;
    }
}
