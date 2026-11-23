#include "usdcc/usd/prim_icons.h"

#include "usdcc/ui/theme.h"

#include <pxr/base/tf/type.h>
#include <pxr/usd/usd/schemaRegistry.h>

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <array>
#include <cstring>
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
// row's white (app.qss, QTreeView::item:selected).
QByteArray iconColor() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::Icon).name().toLatin1(); }
QByteArray selectedIconColor() { return usdcc::ui::theme::color(usdcc::ui::theme::Token::TextStrong).name().toLatin1(); }
// The badges' colour, on selected rows too, is the theme's accent
// (usdcc::ui::theme::accent()), looked up whenever one is drawn.
// An inactive prim's icon, as faded as its name.
constexpr qreal kInactiveOpacity = 0.35;
// The badge's circle in the icon's 24-unit grid, at the lower left: cleared
// before the badge is drawn, so it reads on any row background.
constexpr qreal kBadgeCenterX = 6.5;
constexpr qreal kBadgeCenterY = 17.5;
constexpr qreal kBadgeRadius = 6.75;

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
// is replaced by the colour for the icon's mode before rendering. With a
// badge (another SVG), a circle at the lower left is cleared and the badge
// drawn in it.
class SvgIconEngine final : public QIconEngine {
public:
    SvgIconEngine(QByteArray svg, QByteArray badge, bool inactive)
        : m_svg(std::move(svg)), m_badge(std::move(badge)), m_inactive(inactive) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        // Through a pixmap: clearing the badge's circle must not clear what
        // is under the icon.
        const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override {
        // The colours are part of the key: new ones draw new icons and badges.
        const QString badgeColor = m_badge.isEmpty() ? QString() : usdcc::ui::theme::accent().name();
        const QString key = QStringLiteral("%1x%2@%3/%4/%5/%6")
                                .arg(size.width())
                                .arg(size.height())
                                .arg(scale)
                                .arg(mode)
                                .arg(badgeColor, QString::fromLatin1(iconColor() + selectedIconColor()));
        const auto cached = m_pixmaps.constFind(key);
        if (cached != m_pixmaps.constEnd()) return *cached;
        QPixmap pixmap(size * scale);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(scale);
        {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            draw(painter, QRectF(QPointF(), size), mode, badgeColor.toLatin1());
        }
        m_pixmaps.insert(key, pixmap);
        return pixmap;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    QIconEngine* clone() const override { return new SvgIconEngine(m_svg, m_badge, m_inactive); }
    QString key() const override { return QStringLiteral("usdcc-prim-icon"); }

private:
    void draw(QPainter& painter, const QRectF& rect, QIcon::Mode mode, const QByteArray& badgeColor) {
        if (m_inactive || mode == QIcon::Disabled) painter.setOpacity(kInactiveOpacity);
        renderer(m_svg, m_renderers, mode, iconColor(), selectedIconColor()).render(&painter, rect);
        if (m_badge.isEmpty()) return;
        const qreal unit = rect.width() / 24.0;
        const QPointF center(rect.left() + kBadgeCenterX * unit, rect.top() + kBadgeCenterY * unit);
        painter.save();
        painter.setOpacity(1.0);
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawEllipse(center, kBadgeRadius * unit, kBadgeRadius * unit);
        painter.restore();
        renderer(m_badge, m_badgeRenderers, mode, badgeColor, badgeColor).render(&painter, rect);
    }

    struct Renderer {
        std::unique_ptr<QSvgRenderer> svg;
        QByteArray color;  // what currentColor was replaced with
    };
    using Renderers = std::array<Renderer, 2>;  // normal, selected

    static QSvgRenderer& renderer(const QByteArray& source, Renderers& renderers, QIcon::Mode mode,
                                  const QByteArray& color, const QByteArray& selectedColor) {
        const bool selected = mode == QIcon::Selected;
        Renderer& renderer = renderers[selected ? 1 : 0];
        const QByteArray& wanted = selected ? selectedColor : color;
        if (!renderer.svg || renderer.color != wanted) {
            QByteArray svg = source;
            svg.replace("currentColor", wanted);
            renderer.svg = std::make_unique<QSvgRenderer>(svg);
            renderer.color = wanted;
        }
        return *renderer.svg;
    }

    QByteArray m_svg;
    QByteArray m_badge;
    bool m_inactive;
    Renderers m_renderers;
    Renderers m_badgeRenderers;
    QHash<QString, QPixmap> m_pixmaps;
};

QByteArray readIcon(const QString& name) {
    QFile file(QStringLiteral(":/usdcc/prim-icons/%1.svg").arg(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const char* badgeFile(Badge badge) {
    switch (badge) {
        case Badge::Reference: return "badge-reference";
        case Badge::Payload: return "badge-payload";
        case Badge::Instance: return "badge-instance";
        case Badge::None: break;
    }
    return "";
}

// Pipelines' icons by schema type name (setTypeIcon()), and the caches they invalidate.
std::unordered_map<std::string, QString>& typeIconFiles() {
    static std::unordered_map<std::string, QString> files;
    return files;
}
std::unordered_map<std::string, QString>& resolvedNames() {
    static std::unordered_map<std::string, QString> resolved;
    return resolved;
}
std::array<QHash<QString, QIcon>, 8>& iconCache() {
    static std::array<QHash<QString, QIcon>, 8> icons;  // by inactive and badge
    return icons;
}
}  // namespace

Badge badgeFor(const PXR_NS::UsdPrim& prim) {
    if (!prim) return Badge::None;
    if (prim.IsInstance() || prim.IsInstanceProxy()) return Badge::Instance;
    if (prim.HasAuthoredPayloads()) return Badge::Payload;
    if (prim.HasAuthoredReferences()) return Badge::Reference;
    return Badge::None;
}

void setTypeIcon(const std::string& typeName, const QString& file) {
    if (file.isEmpty()) {
        typeIconFiles().erase(typeName);
    } else {
        typeIconFiles()[typeName] = file;
    }
    resolvedNames().clear();
    for (QHash<QString, QIcon>& cache : iconCache()) cache.clear();
    iconsChanged()();
}

QString typeIcon(const std::string& typeName) {
    const auto found = typeIconFiles().find(typeName);
    return found == typeIconFiles().end() ? QString() : found->second;
}

usdcc::core::Signal<>& iconsChanged() {
    static usdcc::core::Signal<> signal;
    return signal;
}

QString iconNameForType(const PXR_NS::TfToken& typeName) {
    std::unordered_map<std::string, QString>& resolved = resolvedNames();
    const auto found = resolved.find(typeName.GetString());
    if (found != resolved.end()) return found->second;

    QString name = QString::fromLatin1(kUnknownIcon);
    // A pipeline's icon for the name itself, even for a type OpenUSD doesn't know.
    if (const auto own = typeIconFiles().find(typeName.GetString()); own != typeIconFiles().end()) {
        name = QLatin1String(kFileIconPrefix) + own->second;
        resolved.emplace(typeName.GetString(), name);
        return name;
    }
    const PXR_NS::TfType type = PXR_NS::UsdSchemaRegistry::GetTypeFromName(typeName);
    // Breadth first: the nearest base with an icon wins.
    std::deque<PXR_NS::TfType> pending;
    if (!type.IsUnknown()) pending.push_back(type);
    while (!pending.empty()) {
        const PXR_NS::TfType current = pending.front();
        pending.pop_front();
        const std::string schemaName = PXR_NS::UsdSchemaRegistry::GetSchemaTypeName(current).GetString();
        // A pipeline's icon for a base, before the built-in ones.
        if (const auto own = typeIconFiles().find(schemaName); !schemaName.empty() && own != typeIconFiles().end()) {
            name = QLatin1String(kFileIconPrefix) + own->second;
            break;
        }
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

QIcon icon(const QString& name, bool inactive, Badge badge) {
    static const bool initialized = (initPrimIconResources(), true);
    Q_UNUSED(initialized);
    QHash<QString, QIcon>& cache = iconCache()[(inactive ? 1 : 0) + 2 * static_cast<int>(badge)];
    const auto cached = cache.constFind(name);
    if (cached != cache.constEnd()) return *cached;

    QIcon result;
    QByteArray svg;
    if (name.startsWith(QLatin1String(kFileIconPrefix))) {
        // A pipeline's icon: an SVG is drawn as ours are; another image as it is.
        const QString file = name.mid(int(std::strlen(kFileIconPrefix)));
        if (file.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
            QFile source(file);
            if (source.open(QIODevice::ReadOnly)) svg = source.readAll();
        } else {
            result = QIcon(file);
        }
    } else {
        svg = readIcon(name);
    }
    if (!svg.isEmpty()) {
        result = QIcon(new SvgIconEngine(std::move(svg),
                                         badge == Badge::None ? QByteArray() : readIcon(QLatin1String(badgeFile(badge))),
                                         inactive));
    }
    cache.insert(name, result);
    return result;
}

QIcon icon(const PXR_NS::UsdPrim& prim) { return icon(iconName(prim), !prim.IsActive(), badgeFor(prim)); }

}  // namespace usdcc::usd::prim_icons
