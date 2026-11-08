// How the Attribute Editor writes an asset path (docs/ATTRIBUTE_EDITOR.md §4): relative to the
// layer it's written to, or absolute when it can't be.
#include "test_harness.h"
#include "test_settings.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/properties/property_editor.h"

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

using usdcc::usd::properties::assetPathForFile;
using usdcc::usd::properties::createPropertyEditor;
using usdcc::usd::properties::EditorHints;
using usdcc::usd::properties::PropertyEditor;

USDCC_TEST(asset_path_for_file) {
    CHECK(assetPathForFile("C:/show/maps/wood.png", "C:/show") == "./maps/wood.png", "below the layer: %s", qPrintable(assetPathForFile("C:/show/maps/wood.png", "C:/show")));
    CHECK(assetPathForFile("C:/show/wood.png", "C:/show") == "./wood.png", "beside it");
    CHECK(assetPathForFile("C:/maps/wood.png", "C:/show/scenes") == "../../maps/wood.png", "above it: %s", qPrintable(assetPathForFile("C:/maps/wood.png", "C:/show/scenes")));
    CHECK(assetPathForFile("C:/maps/wood.png", "") == "C:/maps/wood.png", "absolute for a layer that isn't saved");
#ifdef Q_OS_WIN
    // No relative path reaches another drive; "./D:/..." wouldn't resolve.
    CHECK(assetPathForFile("D:/maps/wood.png", "C:/show") == "D:/maps/wood.png", "absolute on another drive: %s", qPrintable(assetPathForFile("D:/maps/wood.png", "C:/show")));
#endif
}

// The value comes from a sublayer in another folder, and the edit goes to the root layer:
// what's written has to be relative to the root, which is where USD will resolve it from.
USDCC_TEST(asset_path_relative_to_the_edit_target) {
    TestSettings settings;
    QTemporaryDir temp;
    // Its own, as theme::apply() would make: the editor binds its sizes to one, and the
    // first use would otherwise make one for good, which later tests can't replace.
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()), temp.filePath(QStringLiteral("ui.ini")));
    const QString root = temp.path();
    QDir(root).mkpath("sub");
    QFile texture(root + "/sub/tex.png");
    texture.open(QIODevice::WriteOnly);
    texture.close();
    {
        QFile layer(root + "/sub/layer.usda");
        layer.open(QIODevice::WriteOnly);
        layer.write("#usda 1.0\ndef \"P\"\n{\n    asset file = @./tex.png@\n}\n");
    }
    {
        QFile layer(root + "/root.usda");
        layer.open(QIODevice::WriteOnly);
        layer.write("#usda 1.0\n(\n    subLayers = [@./sub/layer.usda@]\n)\n");
    }
    UsdStageRefPtr stage = UsdStage::Open((root + "/root.usda").toStdString());
    const UsdAttribute attribute = stage->GetPrimAtPath(SdfPath("/P")).GetAttribute(TfToken("file"));
    CHECK(attribute, "the attribute");
    if (!attribute) return;
    CHECK(stage->GetEditTarget().GetLayer() == stage->GetRootLayer(), "editing the root layer");

    const EditorHints hints = EditorHints::forAttribute(attribute);
    std::unique_ptr<PropertyEditor> editor(createPropertyEditor(attribute.GetTypeName(), hints));
    VtValue value;
    attribute.Get(&value);
    editor->setValue(value);
    QString written;
    QObject::connect(editor.get(), &PropertyEditor::valueEdited, [&written](const VtValue& edited) {
        written = QString::fromStdString(edited.UncheckedGet<SdfAssetPath>().GetAssetPath());
    });
    const auto toggle = [&editor]() -> QToolButton* {
        for (QToolButton* button : editor->findChildren<QToolButton*>()) {
            if (button->text() == "Rel" || button->text() == "Abs") return button;
        }
        return nullptr;
    };
    CHECK(toggle() && toggle()->text() == "Rel", "it starts relative");
    if (!toggle()) return;

    toggle()->click();
    const QString absolute = QDir::cleanPath(root + "/sub/tex.png");
    CHECK(written == absolute, "absolute is the file it resolved to: %s", qPrintable(written));
    toggle()->click();
    CHECK(written == "./sub/tex.png", "relative again is relative to the root layer, where it's written: %s", qPrintable(written));
}
