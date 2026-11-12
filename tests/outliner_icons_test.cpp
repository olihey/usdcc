// The Outliner's prim icons (docs/PRIM_ICONS.md): every concrete prim type
// OpenUSD ships has an icon of its own that renders, typeless prims, classes
// and overs get theirs, and the active toggle (an eye) sits at the row's right edge
// (OutlinerItemDelegate), where a click toggles it. Runs in usdcc_ui_tests
// (Qt's offscreen platform).
#include "test_harness.h"

#include "usdcc/ui/scale_controller.h"
#include "usdcc/usd/outliner_item_delegate.h"
#include "usdcc/usd/prim_icons.h"
#include "usdcc/usd/usd_prim_tree_model.h"

#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/typed.h>

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTreeView>

#include <algorithm>
#include <set>
#include <string>

using usdcc::usd::OutlinerItemDelegate;
using usdcc::usd::UsdPrimTreeModel;
namespace prim_icons = usdcc::usd::prim_icons;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// Whether the icon draws anything at `size` (in the given mode).
bool draws(const QIcon& icon, int size, QIcon::Mode mode = QIcon::Normal) {
    const QImage image = icon.pixmap(QSize(size, size), 2.0, mode).toImage();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) return true;
        }
    }
    return false;
}

void click(QWidget* widget, const QPoint& at) {
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, at, widget->mapToGlobal(at), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    }
}
}  // namespace

USDCC_TEST(prim_icons_every_concrete_type) {
    std::set<std::string> missing;
    std::set<std::string> blank;
    std::set<TfType> types;
    TfType::Find<UsdTyped>().GetAllDerivedTypes(&types);
    for (const TfType& type : types) {
        if (!UsdSchemaRegistry::IsConcrete(type)) continue;
        const TfToken name = UsdSchemaRegistry::GetSchemaTypeName(type);
        // The experimental execIr types have no icons of their own: they
        // take their base type's.
        if (name.GetString().rfind("Ir", 0) == 0) continue;
        const QString icon = prim_icons::iconNameForType(name);
        if (icon == QLatin1String("circle-dashed")) missing.insert(name.GetString());
        if (!draws(prim_icons::icon(icon), 16)) blank.insert(name.GetString());
    }
    std::string missingList, blankList;
    for (const auto& name : missing) missingList += name + " ";
    for (const auto& name : blank) blankList += name + " ";
    CHECK(missing.empty(), "types without an icon of their own: %s", missingList.c_str());
    CHECK(blank.empty(), "icons that draw nothing: %s", blankList.c_str());

    CHECK(prim_icons::iconNameForType(TfToken("RectLight")) == "rect-light" &&
              prim_icons::iconNameForType(TfToken("Cylinder_1")) == "cylinder",
          "a light and a versioned schema");
    CHECK(prim_icons::iconNameForType(TfToken("NoSuchType")) == "circle-dashed", "an unknown type");
    CHECK(prim_icons::icon(QStringLiteral("no-such-icon")).isNull(), "no icon for a missing file");
}

USDCC_TEST(prim_icons_for_prims) {
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    stage->DefinePrim(SdfPath("/World/box"), TfToken("Cube"));
    stage->DefinePrim(SdfPath("/World/plain"));
    stage->CreateClassPrim(SdfPath("/_class_Prop"));
    stage->OverridePrim(SdfPath("/patch"));
    const UsdPrim off = stage->DefinePrim(SdfPath("/World/off"), TfToken("Sphere"));
    off.SetActive(false);

    auto name = [&](const char* path) { return prim_icons::iconName(stage->GetPrimAtPath(SdfPath(path))); };
    CHECK(name("/World") == "xform-axes" && name("/World/box") == "cube", "typed prims: %s %s",
          qPrintable(name("/World")), qPrintable(name("/World/box")));
    CHECK(name("/World/plain") == "circle-dashed", "a typeless def: %s", qPrintable(name("/World/plain")));
    CHECK(name("/_class_Prop") == "template", "a class: %s", qPrintable(name("/_class_Prop")));
    CHECK(name("/patch") == "square-dashed", "a bare over: %s", qPrintable(name("/patch")));

    const QIcon box = prim_icons::icon(stage->GetPrimAtPath(SdfPath("/World/box")));
    CHECK(draws(box, 16) && draws(box, 16, QIcon::Selected), "draws, and in white when selected");
    CHECK(draws(prim_icons::icon(off), 16), "an inactive prim's icon still draws, faded");

    // A reference: the same icon with the badge at its lower left.
    const UsdPrim chair = stage->DefinePrim(SdfPath("/World/chair"), TfToken("Cube"));
    chair.GetReferences().AddInternalReference(SdfPath("/World/box"));
    const QIcon referenced = prim_icons::icon(chair);
    const QImage plain = box.pixmap(QSize(16, 16), 2.0).toImage();
    const QImage badged = referenced.pixmap(QSize(16, 16), 2.0).toImage();
    CHECK(prim_icons::iconName(chair) == "cube" && referenced.cacheKey() != box.cacheKey() && plain != badged,
          "a prim with a reference gets the badged icon");
    // Only the lower-left quarter changes.
    CHECK(plain.copy(18, 0, 14, 32) == badged.copy(18, 0, 14, 32) &&
              plain.copy(0, 0, 32, 12) == badged.copy(0, 0, 32, 12) &&
              plain.copy(0, 18, 16, 14) != badged.copy(0, 18, 16, 14),
          "the badge is in the lower left");
    // In the accent blue, the rest of the icon grey.
    auto bluest = [](const QImage& image) {
        int best = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = image.pixel(x, y);
                if (qAlpha(pixel) > 128) best = std::max(best, qBlue(pixel) - qRed(pixel));
            }
        }
        return best;
    };
    CHECK(bluest(badged.copy(0, 18, 16, 14)) > 120 && bluest(plain) < 20, "a blue badge: %d, grey icon: %d",
          bluest(badged.copy(0, 18, 16, 14)), bluest(plain));

    UsdPrimTreeModel model;
    model.setStage(stage);
    const QModelIndex world = model.indexForPath(SdfPath("/World"));
    CHECK(model.data(world, Qt::DecorationRole).value<QIcon>().cacheKey() == prim_icons::icon(QStringLiteral("xform-axes")).cacheKey(),
          "the model shows the icon");
}

USDCC_TEST(outliner_checkbox_on_the_right) {
    QTemporaryDir settings;
    usdcc::ui::UiScaleController scale(*static_cast<QApplication*>(QCoreApplication::instance()),
                                       settings.filePath(QStringLiteral("ui.ini")));
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/box"), TfToken("Cube"));

    UsdPrimTreeModel model;
    model.setStage(stage);
    QTreeView view;
    view.setModel(&model);
    auto* delegate = new OutlinerItemDelegate(&view);
    view.setItemDelegate(delegate);
    view.resize(320, 200);
    view.show();
    QApplication::processEvents();

    const QModelIndex box = model.indexForPath(SdfPath("/box"));
    QStyleOptionViewItem option;
    option.initFrom(view.viewport());
    option.widget = &view;
    option.rect = view.visualRect(box);
    option.decorationSize = view.iconSize();
    option.showDecorationSelected = true;
    const QRect check = delegate->checkRect(option, box);
    CHECK(check.isValid() && option.rect.right() - check.right() < 12 && check.left() > option.rect.center().x(),
          "the box is at the row's right edge: row %d..%d, box %d..%d", option.rect.left(), option.rect.right(),
          check.left(), check.right());

    // The model asks rather than edits (the panel makes it an undo step);
    // here the request is carried out directly.
    int requests = 0;
    QObject::connect(&model, &UsdPrimTreeModel::activeChangeRequested, [&](const SdfPath& path, bool active) {
        ++requests;
        stage->GetPrimAtPath(path).SetActive(active);
    });

    // A click where Qt draws its box, left of the icon, doesn't toggle it.
    click(view.viewport(), QPoint(option.rect.left() + 8, option.rect.center().y()));
    CHECK(requests == 0, "no request from a click on the name");
    CHECK(stage->GetPrimAtPath(SdfPath("/box")).IsActive(), "a click on the name leaves it active");
    CHECK(!prim_icons::icon(QStringLiteral("eye")).isNull() && !prim_icons::icon(QStringLiteral("eye-off")).isNull(),
          "the eye icons are there");
    const QImage activeEye = view.viewport()->grab(check).toImage();
    click(view.viewport(), check.center());
    CHECK(!stage->GetPrimAtPath(SdfPath("/box")).IsActive(), "a click on the eye deactivates the prim");
    const QImage inactiveEye = view.viewport()->grab(check).toImage();
    CHECK(activeEye != inactiveEye, "the eye is struck through once the prim is inactive");
    click(view.viewport(), check.center());
    CHECK(stage->GetPrimAtPath(SdfPath("/box")).IsActive(), "and another activates it again");
    CHECK(view.viewport()->grab(check).toImage() == activeEye, "and the eye is open again");
}
