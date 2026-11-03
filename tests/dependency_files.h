// A small set of files for the dependency tests (dependencies_test.cpp,
// dependencies_panel_test.cpp): root.usda with two sublayers (one missing),
// chair.usda referenced from three prims and from two instances, its own
// leg.usda reference (nested), a payload, a missing reference and a texture.
#pragma once

#include <QFile>
#include <QTemporaryDir>

namespace usdcc::test {

inline void writeDependencyFiles(const QTemporaryDir& dir) {
    auto write = [&dir](const char* name, const char* text) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    };
    write("leg.usda", "#usda 1.0\ndef Cube \"Leg\"\n{\n}\n");
    write("chair.usda", R"(#usda 1.0
(
    defaultPrim = "Chair"
)

def Xform "Chair"
{
    def Xform "Leg" (
        references = @./leg.usda@</Leg>
    )
    {
    }
}
)");
    write("heavy.usda", "#usda 1.0\n(\n    defaultPrim = \"Heavy\"\n)\n\ndef Xform \"Heavy\"\n{\n}\n");
    write("sub.usda", "#usda 1.0\ndef Xform \"FromSub\" (\n    references = @./chair.usda@\n)\n{\n}\n");
    write("wood.png", "");
    write("root.usda", R"(#usda 1.0
(
    subLayers = [@./sub.usda@, @./nosub.usda@]
)

def Xform "A" (
    references = @./chair.usda@
)
{
}

def Xform "B" (
    references = @./chair.usda@
)
{
}

def Xform "P" (
    payload = @./heavy.usda@
)
{
}

def Xform "M" (
    references = @./missing.usda@
)
{
}

def Xform "I1" (
    instanceable = true
    references = @./chair.usda@
)
{
}

def Xform "I2" (
    instanceable = true
    references = @./chair.usda@
)
{
}

def Xform "T"
{
    asset tex = @./wood.png@
}
)");
}

}  // namespace usdcc::test
