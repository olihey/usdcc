#include "usdcc/usd/physics_bake.h"

#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/sim_cache.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdPhysics/tokens.h>

#include <algorithm>
#include <cmath>
#include <map>

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

// The spec for `name` on `prim`, made if needed, with any samples or default it had gone.
SdfAttributeSpecHandle freshAttribute(const SdfPrimSpecHandle& prim, const TfToken& name, const SdfValueTypeName& type,
                                      SdfVariability variability = SdfVariabilityVarying) {
    const SdfPath path = prim->GetPath().AppendProperty(name);
    SdfAttributeSpecHandle spec = prim->GetLayer()->GetAttributeAtPath(path);
    if (!spec) return SdfAttributeSpec::New(prim, name, type, variability);
    spec->ClearInfo(SdfFieldKeys->TimeSamples);
    spec->ClearDefaultValue();
    return spec;
}

// Whether a local matrix is more than scale, rotation and translation.
bool sheared(const GfMatrix4d& local) {
    const GfTransform transform(local);
    GfMatrix4d rotateTranslate(1.0);
    rotateTranslate.SetRotate(transform.GetRotation());
    rotateTranslate.SetTranslateOnly(transform.GetTranslation());
    const GfMatrix4d scaleShear = local * rotateTranslate.GetInverse();
    const GfVec3d scale = transform.GetScale();
    const double size = std::max({std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2]), 1e-12});
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (row != column && std::abs(scaleShear[row][column]) > 1e-7 * size) return true;
        }
    }
    return false;
}

bool chosen(const SdfPath& body, const std::vector<SdfPath>& prims) {
    if (prims.empty()) return true;
    return std::any_of(prims.begin(), prims.end(), [&body](const SdfPath& prim) { return body.HasPrefix(prim); });
}

}  // namespace

bool cacheRange(PhysicsSession& session, const TimeRange& range, const std::function<bool(int, int)>& progress) {
    const double first = std::round(range.start), last = std::round(range.end);
    const int total = std::max(0, int(last - first));
    const std::optional<TimeRange> cached = session.cachedRange();
    if (!session.hasWorld() || !cached || cached->start != first) {
        if (!session.start(first)) return false;
    }
    for (double frame = session.cachedRange()->end + 1.0; frame <= last + 1e-9; frame += 1.0) {
        if (!session.stepTo(frame)) return false;
        if (progress && !progress(int(frame - first), total)) return false;
    }
    return true;
}

std::vector<SdfPath> bodiesToBake(const PhysicsSession& session, const BakeOptions& options) {
    std::vector<SdfPath> bodies;
    for (const BodyDesc& body : session.scene().bodies) {
        if (!body.kinematic && chosen(body.prim, options.prims)) bodies.push_back(body.prim);
    }
    return bodies;
}

BakeResult bakeFromCache(const UsdStageRefPtr& stage, const PhysicsSession& session, const BakeOptions& options) {
    BakeResult result;
    const std::optional<TimeRange> frames = session.cachedRange();
    if (!stage || !frames) return result;
    result.frames = frames;
    const std::vector<SdfPath> bodies = bodiesToBake(session, options);
    if (bodies.empty()) return result;

    // Everything is read first, with the cache composed: a parent that is itself a body is
    // where the cache has it, which is where it will be once baked too.
    std::vector<double> times;
    for (double t = frames->start; t <= frames->end + 1e-9; t += 1.0) times.push_back(t);
    std::map<SdfPath, std::vector<GfMatrix4d>> locals;
    for (const SdfPath& path : bodies) locals[path].reserve(times.size());
    for (const double t : times) {
        UsdGeomXformCache xforms{UsdTimeCode(t)};
        for (const SdfPath& path : bodies) {
            const UsdPrim prim = stage->GetPrimAtPath(path);
            const GfMatrix4d world = xforms.GetLocalToWorldTransform(prim);
            const GfMatrix4d parent = xforms.GetParentToWorldTransform(prim);
            locals[path].push_back(world * parent.GetInverse());
        }
    }

    const UsdEditTarget target = stage->GetEditTarget();
    const SdfLayerHandle layer = target.GetLayer();
    {
        SdfChangeBlock block;
        for (const SdfPath& path : bodies) {
            const std::vector<GfMatrix4d>& samples = locals[path];
            const SdfPrimSpecHandle prim = SdfCreatePrimInLayer(layer, target.MapToSpecPath(path));
            if (!prim) {
                result.reports.push_back({path, "couldn't be written in the edit target"});
                continue;
            }
            const bool asMatrix = std::any_of(samples.begin(), samples.end(), sheared);
            VtTokenArray order;
            if (asMatrix) {
                order.push_back(transformOp());
                const SdfAttributeSpecHandle matrix = freshAttribute(prim, transformOp(), SdfValueTypeNames->Matrix4d);
                for (size_t i = 0; i < times.size(); ++i) layer->SetTimeSample(matrix->GetPath(), times[i], samples[i]);
            } else {
                order.insert(order.end(), {translateOp(), orientOp(), scaleOp()});
                const SdfAttributeSpecHandle translate = freshAttribute(prim, translateOp(), SdfValueTypeNames->Double3);
                const SdfAttributeSpecHandle orient = freshAttribute(prim, orientOp(), SdfValueTypeNames->Quatf);
                for (size_t i = 0; i < times.size(); ++i) {
                    const GfTransform transform(samples[i]);
                    layer->SetTimeSample(translate->GetPath(), times[i], transform.GetTranslation());
                    layer->SetTimeSample(orient->GetPath(), times[i],
                                         GfQuatf(transform.GetRotation().GetQuat().GetNormalized()));
                }
                freshAttribute(prim, scaleOp(), SdfValueTypeNames->Float3)
                    ->SetDefaultValue(VtValue(GfVec3f(GfTransform(samples.front()).GetScale())));
            }
            freshAttribute(prim, UsdGeomTokens->xformOpOrder, SdfValueTypeNames->TokenArray, SdfVariabilityUniform)
                ->SetDefaultValue(VtValue(order));
            if (options.markKinematic) {
                freshAttribute(prim, UsdPhysicsTokens->physicsKinematicEnabled, SdfValueTypeNames->Bool)
                    ->SetDefaultValue(VtValue(true));
            }
            result.baked.push_back(path);
        }
    }

    // A stronger layer than the edit target (other than the cache) with its own
    // xformOpOrder would hide the bake.
    for (const SdfPath& path : result.baked) {
        const UsdAttribute order = UsdGeomXformable(stage->GetPrimAtPath(path)).GetXformOpOrderAttr();
        for (const SdfPropertySpecHandle& spec : order.GetPropertyStack()) {
            if (SimCache::isCacheLayer(spec->GetLayer())) continue;
            if (spec->GetLayer() != layer) {
                result.reports.push_back({path, "a stronger layer (" + spec->GetLayer()->GetDisplayName() +
                                                    ") sets its xformOpOrder: the bake doesn't show"});
            }
            break;
        }
    }
    return result;
}

}  // namespace usdcc::usd::physics
