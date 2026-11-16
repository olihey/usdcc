#include "usdcc/usd/sim_cache.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include <algorithm>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::physics {

namespace {
const TfToken& translateOp() {
    static const TfToken token("xformOp:translate:sim");
    return token;
}
const TfToken& orientOp() {
    static const TfToken token("xformOp:orient:sim");
    return token;
}
const TfToken& scaleOp() {
    static const TfToken token("xformOp:scale:sim");
    return token;
}
const TfToken& transformOp() {
    static const TfToken token("xformOp:transform:sim");
    return token;
}

SdfAttributeSpecHandle attribute(const SdfPrimSpecHandle& prim, const TfToken& name, const SdfValueTypeName& type,
                                 SdfVariability variability = SdfVariabilityVarying) {
    if (SdfAttributeSpecHandle existing = prim->GetLayer()->GetAttributeAtPath(prim->GetPath().AppendProperty(name))) {
        return existing;
    }
    return SdfAttributeSpec::New(prim, name, type, variability);
}
}  // namespace

bool SimCache::isCacheLayerIdentifier(const std::string& identifier) {
    return TfStringStartsWith(identifier, "anon:") && TfStringEndsWith(identifier, std::string(":") + kTag);
}

bool SimCache::isCacheLayer(const SdfLayerHandle& layer) {
    return layer && layer->IsAnonymous() && isCacheLayerIdentifier(layer->GetIdentifier());
}

SimCache::SimCache(UsdStageRefPtr stage) : m_stage(std::move(stage)) {}

SimCache::~SimCache() = default;

void SimCache::prepare(const PhysicsSceneDesc& scene) {
    if (!m_stage) return;
    const SdfLayerHandle session = m_stage->GetSessionLayer();
    if (!session) return;
    if (!m_layer) m_layer = SdfLayer::CreateAnonymous(kTag);
    // Strongest of the session layer's sublayers: the session layer itself, where variant
    // edit mode writes, stays stronger still.
    const std::vector<std::string> sublayers = session->GetSubLayerPaths();
    if (std::find(sublayers.begin(), sublayers.end(), m_layer->GetIdentifier()) == sublayers.end()) {
        session->InsertSubLayerPath(m_layer->GetIdentifier(), 0);
    }
    SdfChangeBlock block;
    for (const BodyDesc& body : scene.bodies) {
        if (body.kinematic) continue;
        const SdfPrimSpecHandle prim = SdfCreatePrimInLayer(m_layer, body.prim);  // an over
        if (!prim) continue;
        VtTokenArray order{UsdGeomXformOpTypes->resetXformStack};
        if (body.sheared) {
            order.push_back(transformOp());
            attribute(prim, transformOp(), SdfValueTypeNames->Matrix4d);
        } else {
            order.insert(order.end(), {translateOp(), orientOp(), scaleOp()});
            attribute(prim, translateOp(), SdfValueTypeNames->Double3);
            attribute(prim, orientOp(), SdfValueTypeNames->Quatf);
            attribute(prim, scaleOp(), SdfValueTypeNames->Float3)->SetDefaultValue(VtValue(GfVec3f(body.scale)));
        }
        attribute(prim, UsdGeomTokens->xformOpOrder, SdfValueTypeNames->TokenArray, SdfVariabilityUniform)
            ->SetDefaultValue(VtValue(order));
    }
}

void SimCache::write(double time, const PhysicsSceneDesc& scene, const std::vector<Pose>& poses) {
    if (!m_layer) return;
    SdfChangeBlock block;
    for (size_t i = 0; i < scene.bodies.size() && i < poses.size(); ++i) {
        const BodyDesc& body = scene.bodies[i];
        if (body.kinematic) continue;
        const GfVec3d position = toStagePosition(scene, poses[i].position);
        const GfQuatd& rotation = poses[i].rotation;
        if (body.sheared) {
            // M = S R T, with S as the body had it at the start frame.
            GfMatrix4d rotateTranslate(1.0);
            rotateTranslate.SetRotate(GfRotation(rotation));
            rotateTranslate.SetTranslateOnly(position);
            m_layer->SetTimeSample(body.prim.AppendProperty(transformOp()), time, body.scaleShear * rotateTranslate);
        } else {
            m_layer->SetTimeSample(body.prim.AppendProperty(translateOp()), time, position);
            m_layer->SetTimeSample(body.prim.AppendProperty(orientOp()), time, GfQuatf(rotation));
        }
    }
    m_range = m_range ? united(*m_range, TimeRange{time, time}) : TimeRange{time, time};
}

void SimCache::clear() {
    m_range.reset();
    if (m_layer && !m_layer->IsEmpty()) m_layer->Clear();
}

}  // namespace usdcc::usd::physics
