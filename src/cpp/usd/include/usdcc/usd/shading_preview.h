#pragma once

#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdShade/material.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace usdcc::usd {

// The Material Editor's preview of a material on a shape (docs/MATERIAL_EDITOR.md §7.2–§7.6,
// §7.9, §7.10).
// Qt-free.
//
// A stage of its own that shares the scene's layers: it opens the scene's root layer, masked
// to the material and the preview's own prims, with a session layer of its own. That session
// layer sublayers the scene's session layer, so the material composes exactly as it does in
// the scene, and a layer with the shape and the light. Every edit to the scene's layers
// reaches it through USD's own change notices; nothing written here reaches the scene.
//
// Instead of a shape file, a prim of the scene can show the material: it's added to the mask,
// and renders where it is. What the preview overrides on it is in a layer stronger than the
// scene's session layer.
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

    // A prim of the scene shown instead of the shape file, in place (§7.10): previewablePrim()
    // of it. Empty shows the shape file again. False, and why, if it can't be shown; the shape
    // file then shows. If something in it has the material in the scene, its bindings stay as
    // they are; otherwise all of it shows the material.
    bool setScenePrim(const PXR_NS::SdfPath& prim, std::string* why = nullptr);
    const PXR_NS::SdfPath& scenePrim() const { return m_scenePrim; }
    bool scenePrimKeepsBindings() const { return m_use.keepsBindings; }

    void setEnvironment(const Environment& environment);
    const Environment& environment() const { return m_environment; }

    // The UV sets the material reads that the shape doesn't have, and so got as a copy of its
    // own (§7.9): uvSetSource(), usually "st". None on a prim of the scene.
    const std::vector<std::string>& filledUvSets() const { return m_filledUvSets; }
    const std::string& uvSetSource() const { return m_uvSetSource; }
    // The primvars the material reads that something showing it doesn't have.
    const std::vector<std::string>& missingPrimvars() const { return m_missingPrimvars; }

    // Shows one shader output on the shape instead of the material's surface (§7.6), the
    // path of an outputs: attribute of a shader in the shown material. Empty shows the
    // material again. False, and why, if it can't be shown.
    bool setPreviewedOutput(const PXR_NS::SdfPath& output, std::string* why = nullptr);
    const PXR_NS::SdfPath& previewedOutput() const { return m_output; }

    // Keeps what's per stage in step with the scene: muted layers and load rules, and the
    // shape's scale for the scene's units. Cheap when nothing changed. The preview itself is
    // always Y-up, as the viewport's camera is: only a shape authored Z-up is turned.
    // Returns how much bigger the shape got about the preview's origin, 1 unless the units
    // changed: a view of it that's scaled along (§7.3) shows the same picture. Always 1 for a
    // prim of the scene, which isn't scaled.
    double syncWithSource();
    // Something in the scene changed, as its change notice says: what depends on the material's
    // network or the scene prim follows. A scene prim that's gone gives way to the shape file.
    void sceneChanged(const std::vector<PXR_NS::SdfPath>& resynced, const std::vector<PXR_NS::SdfPath>& changed);

    // The shape's bounds in the preview stage, for framing it.
    PXR_NS::GfRange3d shapeBounds(PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default()) const;

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
    // From the shape's units to the scene's, the last time the shape was scaled.
    double shapeScale() const;
    void writeBinding();
    void writeEnvironment();
    // Switches the shape's lights off under an HDRI, on otherwise, in m_scene.
    void writeShapeLights();
    // Puts the shape's scale into its area lights' own size instead of their transform: Storm
    // ignores a light's scale but not its distance (§7.3).
    void writeShapeLightUnits(float scale);
    // Takes back what writeShapeLights() wrote for the last shape.
    void clearShapeLights();
    void applyMask();
    // What the scene prim's bindings are, from the scene. False if it can't be shown.
    bool readScenePrim();
    // The scene prim's visibility and binding, in m_over.
    void writeScenePrim();
    // Gives the shape the UV sets the material reads, and finds what's missing.
    void updatePrimvars(bool force);
    void clearUvSets();
    // What shows the material: on the shape file's, in the preview stage.
    std::vector<PXR_NS::UsdPrim> shapeGprims() const;
    bool turnedZUp() const;

    // What the scene prim holds, in the scene.
    struct ScenePrimUse {
        PXR_NS::SdfPath shown;                    // previewablePrim(): what's in the mask
        bool keepsBindings = false;               // something in it has the material
        std::set<PXR_NS::SdfPath> materials;      // what else the mask needs for its bindings
        std::vector<PXR_NS::SdfPath> showing;     // the gprims showing the material
    };

    PXR_NS::UsdStageRefPtr m_source;
    PXR_NS::SdfLayerRefPtr m_session;  // the preview stage's session layer: what only it sees
    // What's overridden on the scene prim: the strongest sublayer of m_session, above the
    // scene's session layer.
    PXR_NS::SdfLayerRefPtr m_over;
    PXR_NS::SdfLayerRefPtr m_scene;  // the shape and the light, the weakest sublayer of m_session
    PXR_NS::UsdStageRefPtr m_stage;
    PXR_NS::SdfPath m_material;
    PXR_NS::SdfPath m_output;
    std::string m_shapeFile;
    PXR_NS::SdfPath m_scenePrim;  // as it was asked for
    ScenePrimUse m_use;
    Environment m_environment;
    bool m_shapeZUp = false;          // as the shape file says
    double m_shapeMetersPerUnit = 1;  // the same
    double m_metersPerUnit = 0.01;    // the scene's, the last time the shape was scaled
    bool m_shapeHasLights = false;
    std::vector<PXR_NS::SdfPath> m_shapeLightUnits;  // the properties writeShapeLightUnits() wrote in m_scene
    std::vector<PXR_NS::SdfPath> m_shapeLights;  // what an HDRI switches off: not mesh lights
    bool m_wroteZUp = false;                     // how the dome and key light were last turned

    std::set<PXR_NS::SdfPath> m_network;  // the prims of the material's network
    std::string m_primvarsKey;            // what the rest was last worked out for
    std::vector<PXR_NS::SdfPath> m_filledAttributes;  // in m_scene
    std::vector<std::string> m_filledUvSets;
    std::string m_uvSetSource;
    std::vector<std::string> m_missingPrimvars;
};

// The primvars a material's shaders read, by name: from their definitions, which say what
// they always read (the primary UV set) and which inputs name a primvar, an interface input's
// value when that's where the name comes from (docs/MATERIAL_EDITOR.md §7.9).
struct MaterialPrimvar {
    std::string name;
    bool uv = false;       // read as two numbers: a UV set
    bool reached = false;  // by what the material's outputs are connected to
};
// `network`, if given, gets the prims of the network.
std::vector<MaterialPrimvar> primvarsReadBy(const PXR_NS::UsdShadeMaterial& material, std::set<PXR_NS::SdfPath>* network = nullptr);
// Whether a renderer has a value for it where the geometry doesn't, so it's never missing.
bool rendererSuppliesPrimvar(const std::string& name);

// What the preview shows when `prim` is picked as its shape (§7.10): the prim, the instance's
// root for one inside an instance, the geometry for a subset of it. Invalid, and why, for one
// with no geometry to show.
PXR_NS::UsdPrim previewablePrim(const PXR_NS::UsdPrim& prim, std::string* why = nullptr);

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
