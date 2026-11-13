#pragma once

#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>

#include <optional>
#include <string>
#include <vector>

namespace usdcc::usd {

// The Material Editor's preview of a material on a shape (docs/MATERIAL_EDITOR.md §7.2–§7.6).
// Qt-free.
//
// A stage of its own that shares the scene's layers: it opens the scene's root layer, masked
// to the material and the preview's own prims, with a session layer of its own. That session
// layer sublayers the scene's session layer, so the material composes exactly as it does in
// the scene, and a layer with the shape and the light. Every edit to the scene's layers
// reaches it through USD's own change notices; nothing written here reaches the scene.
class ShadingPreviewStage {
public:
    // Where the preview's prims are: the shape, the light.
    static const PXR_NS::SdfPath& root();

    struct Environment {
        std::string hdriFile;  // empty: a plain grey dome and a key light
        double rotation = 0;   // degrees, about the scene's up axis
        double exposure = 0;
        // Only the shape's own lights: the dome and the key light are switched off.
        bool sceneLights = false;
        bool operator==(const Environment&) const = default;
    };

    explicit ShadingPreviewStage(PXR_NS::UsdStageRefPtr source);
    ~ShadingPreviewStage();
    ShadingPreviewStage(const ShadingPreviewStage&) = delete;
    ShadingPreviewStage& operator=(const ShadingPreviewStage&) = delete;

    const PXR_NS::UsdStageRefPtr& stage() const { return m_stage; }
    const PXR_NS::UsdStageRefPtr& source() const { return m_source; }

    // The Material shown on the shape; empty for none. False, and why, if it isn't a
    // Material of the scene; the shape then shows none.
    bool setMaterial(const PXR_NS::SdfPath& material, std::string* why = nullptr);
    const PXR_NS::SdfPath& material() const { return m_material; }

    // A USD file whose defaultPrim is the shape (§7.3). False, and why, if it can't be
    // opened or has no defaultPrim; the last good shape stays.
    bool setShape(const std::string& file, std::string* why = nullptr);
    const std::string& shape() const { return m_shapeFile; }

    void setEnvironment(const Environment& environment);
    const Environment& environment() const { return m_environment; }

    // Shows one shader output on the shape instead of the material's surface (§7.6), the
    // path of an outputs: attribute of a shader in the shown material. Empty shows the
    // material again. False, and why, if it can't be shown.
    bool setPreviewedOutput(const PXR_NS::SdfPath& output, std::string* why = nullptr);
    const PXR_NS::SdfPath& previewedOutput() const { return m_output; }

    // Keeps what's per stage in step with the scene: muted layers and load rules, and the
    // shape's scale for the scene's units. Cheap when nothing changed. The preview itself is
    // always Y-up, as the viewport's camera is: only a shape authored Z-up is turned.
    void syncWithSource();

    // The shape's bounds in the preview stage, for framing it.
    PXR_NS::GfRange3d shapeBounds() const;

    // The view from the shape file's first perspective Camera (§7.3), in the preview stage's
    // space: the preview looks through it instead of framing the bounds. Nothing when the
    // shape has none.
    struct ShapeCamera {
        PXR_NS::SdfPath path;
        PXR_NS::GfVec3d eye;
        // What the view turns about: the point on its line of sight nearest the middle of
        // what shows the material (collection:preview, else the whole shape).
        PXR_NS::GfVec3d target;
        double verticalFov = 0;  // degrees
    };
    std::optional<ShapeCamera> shapeCamera() const;

    // Whether the shape file has lights of its own (a prim with UsdLuxLightAPI), which
    // Environment::sceneLights can light it with alone. Under an HDRI (without
    // sceneLights) they're switched off, all but mesh lights, which are geometry too.
    bool shapeHasLights() const;

    // The material's surface terminals that something is connected to, such as
    // "outputs:surface" and "outputs:mtlx:surface": what a renderer can choose from.
    std::vector<std::string> surfaceTerminals() const;

private:
    void writeScene();
    void writeShape();
    void writeBinding();
    void writeEnvironment();
    // Switches the shape's lights off under an HDRI, on otherwise, in m_scene.
    void writeShapeLights();
    // Takes back what writeShapeLights() wrote for the last shape.
    void clearShapeLights();
    void applyMask();

    PXR_NS::UsdStageRefPtr m_source;
    PXR_NS::SdfLayerRefPtr m_session;  // the preview stage's session layer: what only it sees
    PXR_NS::SdfLayerRefPtr m_scene;    // the shape and the light, a sublayer of m_session
    PXR_NS::UsdStageRefPtr m_stage;
    PXR_NS::SdfPath m_material;
    PXR_NS::SdfPath m_output;
    std::string m_shapeFile;
    Environment m_environment;
    bool m_shapeZUp = false;          // as the shape file says
    double m_shapeMetersPerUnit = 1;  // the same
    double m_metersPerUnit = 0.01;    // the scene's, the last time the shape was scaled
    bool m_shapeHasLights = false;
    std::vector<PXR_NS::SdfPath> m_shapeLights;  // what an HDRI switches off: not mesh lights
};

// The output of a shader to preview when it's selected: "rgb" if it has one, else its first
// colour, then vector, then surface, then any output; its definition's or authored. Empty if
// it has none, or isn't a Shader.
PXR_NS::SdfPath defaultPreviewOutput(const PXR_NS::UsdPrim& shader);

// What a preview menu lists: each USD file (shapes) or image (HDRIs) in `folders`, by
// name, the first folder that has a name winning. Not recursive.
struct PreviewAsset {
    std::string name;  // the file's name without its extension
    std::string file;
};
std::vector<PreviewAsset> listPreviewAssets(const std::vector<std::string>& folders, const std::vector<std::string>& extensions);
inline const std::vector<std::string>& previewShapeExtensions() {
    static const std::vector<std::string> extensions{".usd", ".usda", ".usdc", ".usdz"};
    return extensions;
}
inline const std::vector<std::string>& previewHdriExtensions() {
    static const std::vector<std::string> extensions{".hdr", ".exr"};
    return extensions;
}

}  // namespace usdcc::usd
