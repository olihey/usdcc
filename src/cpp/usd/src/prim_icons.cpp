#include "usdcc/usd/prim_icons.h"

#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/schemaRegistry.h>

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

// Outside any namespace: Q_INIT_RESOURCE declares a function of its own.
// The icons live in this static library, so the linker would otherwise drop
// them (as with usdcc_ui's theme, see theme.cpp).
static void initPrimIconResources() { Q_INIT_RESOURCE(prim_icons); }

namespace usdcc::usd::prim_icons {

namespace {
// The theme's icon grey (tools/gen_theme_icons.py LIGHT) and a selected
// row's white (dark.qss, QTreeView::item:selected).
constexpr auto kIconColor = "#a0a0a4";
constexpr auto kSelectedColor = "#ffffff";
// An inactive prim's icon, as faded as its name.
constexpr qreal kInactiveOpacity = 0.35;

// Concrete prim types by their schema type name; each light and each
// primitive shape has its own (docs/PRIM_ICONS.md §3).
const std::unordered_map<std::string, const char*>& typeIcons() {
    static const std::unordered_map<std::string, const char*> icons = {
        // Structure
        {"Xform", "xform-axes"},
        {"Scope", "folder"},
        // Geometry
        {"Cube", "cube"},
        {"Sphere", "sphere"},
        {"Cone", "cone"},
        {"Cylinder", "cylinder"},
        {"Cylinder_1", "cylinder"},
        {"Capsule", "capsule"},
        {"Capsule_1", "capsule"},
        {"Plane", "perspective"},
        {"Mesh", "topology-full"},
        {"TetMesh", "pyramid"},
        {"NurbsPatch", "vector"},
        {"BasisCurves", "vector-spline"},
        {"NurbsCurves", "vector-spline"},
        {"HermiteCurves", "vector-spline"},
        {"Points", "grid-dots"},
        {"PointInstancer", "grain"},
        {"GeomSubset", "square-toggle"},
        {"GenerativeProcedural", "cube-spark"},
        // Camera and lights
        {"Camera", "video"},
        {"DistantLight", "arrows-right"},
        {"DomeLight", "sunset"},
        {"DomeLight_1", "sunset"},
        {"SphereLight", "sun-high"},
        {"RectLight", "rect-light"},
        {"DiskLight", "disk-light"},
        {"CylinderLight", "tube-light"},
        {"PortalLight", "window"},
        {"GeometryLight", "prism-light"},
        {"PluginLight", "plug"},
        {"LightFilter", "filter"},
        {"PluginLightFilter", "filter-cog"},
        // Shading
        {"Material", "crystal-ball"},
        {"NodeGraph", "topology-star-3"},
        {"Shader", "inner-shadow-top-right"},
        {"Backdrop", "layout-board"},
        // Skeletons and animation
        {"SkelRoot", "walk"},
        {"Skeleton", "bone"},
        {"SkelAnimation", "keyframes"},
        {"BlendShape", "circle-square"},
        // Volumes and particles
        {"Volume", "cloud"},
        {"OpenVDBAsset", "cloud-fog"},
        {"Field3DAsset", "cloud-fog"},
        {"ParticleField", "sparkles"},
        {"ParticleField3DGaussianSplat", "sparkles"},
        // Render
        {"RenderSettings", "adjustments"},
        {"RenderProduct", "photo"},
        {"RenderVar", "stack-2"},
        {"RenderPass", "movie"},
        // Physics
        {"PhysicsScene", "atom"},
        {"PhysicsJoint", "link"},
        {"PhysicsFixedJoint", "link"},
        {"PhysicsRevoluteJoint", "link"},
        {"PhysicsPrismaticJoint", "link"},
        {"PhysicsSphericalJoint", "link"},
        {"PhysicsDistanceJoint", "link"},
        {"PhysicsCollisionGroup", "category"},
        // Other
        {"SpatialAudio", "volume"},
        {"LODDistanceHeuristic", "ruler-2"},
        {"LODScreenSizeHeuristic", "ruler-2"},
    };
    return icons;
}

// Abstract base types by their C++ name, for types without an icon of their
// own (docs/PRIM_ICONS.md §5).
const std::unordered_map<std::string, const char*>& baseIcons() {
    static const std::unordered_map<std::string, const char*> icons = {
        {"UsdLuxBoundableLightBase", "bulb"},
        {"UsdLuxNonboundableLightBase", "bulb"},
        {"UsdLuxLightFilter", "filter"},
        {"UsdGeomCurves", "vector-spline"},
        {"UsdGeomPointBased", "topology-full"},
        {"UsdGeomGprim", "cube-3d-sphere"},
        {"UsdGeomBoundable", "cube-3d-sphere"},
        {"UsdGeomXformable", "xform-axes"},
        {"UsdGeomImageable", "circle-dashed"},
        {"UsdShadeNodeGraph", "topology-star-3"},
        {"UsdPhysicsJoint", "link"},
        {"UsdRenderSettingsBase", "adjustments"},
        {"UsdVolFieldAsset", "cloud-fog"},
    };
    return icons;
}

const char* const kUnknownIcon = "circle-dashed";

// Draws one SVG, recoloured: Tabler's icons stroke with currentColor, which
// is replaced by the colour for the icon's mode before rendering.
class SvgIconEngine final : public QIconEngine {
public:
    SvgIconEngine(QByteArray svg, bool inactive) : m_svg(std::move(svg)), m_inactive(inactive) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        if (m_inactive || mode == QIcon::Disabled) painter->setOpacity(painter->opacity() * kInactiveOpacity);
        renderer(mode).render(painter, QRectF(rect));
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        const QString key = QStringLiteral("%1x%2@%3/%4").arg(size.width()).arg(size.height()).arg(scale).arg(mode);
        const auto cached = m_pixmaps.constFind(key);
        if (cached != m_pixmaps.constEnd()) return *cached;
        QPixmap pixmap(size * scale);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(scale);
        {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            paint(&painter, QRect(QPoint(), size), mode, state);
        }
        m_pixmaps.insert(key, pixmap);
        return pixmap;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    QIconEngine* clone() const override { return new SvgIconEngine(m_svg, m_inactive); }
    QString key() const override { return QStringLiteral("usdcc-prim-icon"); }

private:
    QSvgRenderer& renderer(QIcon::Mode mode) {
        const bool selected = mode == QIcon::Selected;
        std::unique_ptr<QSvgRenderer>& renderer = m_renderers[selected ? 1 : 0];
        if (!renderer) {
            QByteArray svg = m_svg;
            svg.replace("currentColor", selected ? kSelectedColor : kIconColor);
            renderer = std::make_unique<QSvgRenderer>(svg);
        }
        return *renderer;
    }

    QByteArray m_svg;
    bool m_inactive;
    std::array<std::unique_ptr<QSvgRenderer>, 2> m_renderers;  // normal, selected
    QHash<QString, QPixmap> m_pixmaps;
};
}  // namespace

QString iconNameForType(const PXR_NS::TfToken& typeName) {
    static std::unordered_map<std::string, QString> resolved;
    const auto found = resolved.find(typeName.GetString());
    if (found != resolved.end()) return found->second;

    QString name = QString::fromLatin1(kUnknownIcon);
    const PXR_NS::TfType type = PXR_NS::UsdSchemaRegistry::GetTypeFromName(typeName);
    // Breadth first: the nearest base with an icon wins.
    std::deque<PXR_NS::TfType> pending;
    if (!type.IsUnknown()) pending.push_back(type);
    while (!pending.empty()) {
        const PXR_NS::TfType current = pending.front();
        pending.pop_front();
        const std::string schemaName = PXR_NS::UsdSchemaRegistry::GetSchemaTypeName(current).GetString();
        if (const auto icon = typeIcons().find(schemaName); !schemaName.empty() && icon != typeIcons().end()) {
            name = QString::fromLatin1(icon->second);
            break;
        }
        if (const auto icon = baseIcons().find(current.GetTypeName()); icon != baseIcons().end()) {
            name = QString::fromLatin1(icon->second);
            break;
        }
        for (const PXR_NS::TfType& base : current.GetBaseTypes()) pending.push_back(base);
    }
    resolved.emplace(typeName.GetString(), name);
    return name;
}

QString iconName(const PXR_NS::UsdPrim& prim) {
    if (prim.GetSpecifier() == PXR_NS::SdfSpecifierClass) return QStringLiteral("template");
    if (!prim.IsDefined()) return QStringLiteral("square-dashed");  // an over with no def under it
    return iconNameForType(prim.GetTypeName());
}

QIcon icon(const QString& name, bool inactive) {
    static const bool initialized = (initPrimIconResources(), true);
    Q_UNUSED(initialized);
    static std::array<QHash<QString, QIcon>, 2> icons;  // active, inactive
    QHash<QString, QIcon>& cache = icons[inactive ? 1 : 0];
    const auto cached = cache.constFind(name);
    if (cached != cache.constEnd()) return *cached;

    QIcon result;
    QFile file(QStringLiteral(":/usdcc/prim-icons/%1.svg").arg(name));
    if (file.open(QIODevice::ReadOnly)) result = QIcon(new SvgIconEngine(file.readAll(), inactive));
    cache.insert(name, result);
    return result;
}

QIcon icon(const PXR_NS::UsdPrim& prim) { return icon(iconName(prim), !prim.IsActive()); }

}  // namespace usdcc::usd::prim_icons
