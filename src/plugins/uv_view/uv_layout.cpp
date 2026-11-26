#include "uv_layout.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/utils.h>
#include <pxr/usd/usdUtils/pipeline.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::uv {

namespace {
template <typename Vec>
bool convert(const VtValue& value, VtVec2fArray& out) {
    if (!value.IsHolding<VtArray<Vec>>()) return false;
    const VtArray<Vec>& in = value.UncheckedGet<VtArray<Vec>>();
    out.resize(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) out[i] = GfVec2f(float(in[i][0]), float(in[i][1]));
    return true;
}

// Twice the signed area of a face's UV polygon (shoelace): positive counter-clockwise.
double signedArea(const std::vector<GfVec2f>& corners, std::size_t start, int count) {
    double sum = 0;
    for (int i = 0; i < count; ++i) {
        const GfVec2f& a = corners[start + std::size_t(i)];
        const GfVec2f& b = corners[start + std::size_t((i + 1) % count)];
        sum += double(a[0]) * double(b[1]) - double(b[0]) * double(a[1]);
    }
    return sum;
}

bool inside(const GfVec2f& p, const std::vector<GfVec2f>& corners, std::size_t start, int count) {
    bool in = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const GfVec2f& a = corners[start + std::size_t(i)];
        const GfVec2f& b = corners[start + std::size_t(j)];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
}

// What a texture's coordinates input reads, following its connections a few shaders up to a
// reader's varname or geomprop. The primary UV set when nothing's connected; nothing when it's
// connected to what can't be followed.
std::string setRead(const UsdShadeShader& shader) {
    for (const char* name : {"st", "texcoord"}) {
        UsdShadeInput reading = shader.GetInput(TfToken(name));
        if (!reading) continue;
        if (!reading.HasConnectedSource()) return UsdUtilsGetPrimaryUVSetName().GetString();
        for (int hops = 0; hops < 4; ++hops) {
            const UsdShadeAttributeVector sources = reading.GetValueProducingAttributes();
            if (sources.empty() || UsdShadeUtils::GetType(sources.front().GetName()) != UsdShadeAttributeType::Output) return {};
            const UsdShadeShader upstream(sources.front().GetPrim());
            if (!upstream) return {};
            for (const char* naming : {"varname", "geomprop"}) {
                const UsdShadeInput nameInput = upstream.GetInput(TfToken(naming));
                if (!nameInput) continue;
                const UsdShadeAttributeVector values = nameInput.GetValueProducingAttributes();
                VtValue value;
                if (!values.empty() && values.front().Get(&value)) {
                    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
                    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
                }
                return UsdUtilsGetPrimaryUVSetName().GetString();  // a reader with no name reads the default
            }
            // A node in between, such as a transform of the coordinates: its own coordinates.
            reading = upstream.GetInput(TfToken("in"));
            if (!reading) reading = upstream.GetInput(TfToken("texcoord"));
            if (!reading) return {};
        }
        return {};
    }
    // No coordinates input: the default, the primary set.
    return UsdUtilsGetPrimaryUVSetName().GetString();
}
// A path with <UDIM> in it, anchored to the layer that has it. Ar anchors a relative path only to
// a file that's there, so it's asked about tile 1001's; failing that, the layer's folder.
std::string anchorUdim(const SdfLayerHandle& layer, const std::string& asset) {
    const std::size_t at = asset.find("<UDIM>");
    if (!layer || at == std::string::npos) return layer ? SdfComputeAssetPathRelativeToLayer(layer, asset) : asset;
    std::string probe = asset;
    probe.replace(at, 6, "1001");
    const std::string anchored = SdfComputeAssetPathRelativeToLayer(layer, probe);
    if (anchored != probe && !anchored.empty()) {
        return TfStringCatPaths(TfGetPathName(anchored), TfGetBaseName(asset));
    }
    if (!TfIsRelativePath(asset) || layer->IsAnonymous() || layer->GetRealPath().empty()) return asset;
    return TfNormPath(TfStringCatPaths(TfGetPathName(layer->GetRealPath()), asset));
}
}  // namespace

int udimTile(const GfVec2f& uv) {
    const double u = std::floor(uv[0]), v = std::floor(uv[1]);
    if (u < 0 || u > 9 || v < 0 || !std::isfinite(u) || !std::isfinite(v)) return 0;
    return 1001 + int(u) + 10 * int(v);
}

UvSource readUvSource(const UsdPrim& prim, const std::string& primvar, UsdTimeCode time) {
    UvSource source;
    source.prim = prim ? prim.GetPath() : SdfPath();
    source.primvar = primvar;
    const UsdGeomMesh mesh(prim);
    if (!mesh) {
        source.error = "It isn't a mesh.";
        return source;
    }
    const UsdGeomPrimvar uv = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken(primvar));
    VtValue value;
    if (!uv || !uv.Get(&value, time) || value.IsEmpty()) {
        source.error = "It has no " + primvar + ".";
        return source;
    }
    source.interpolation = uv.GetInterpolation();
    if (source.interpolation != UsdGeomTokens->faceVarying && source.interpolation != UsdGeomTokens->vertex &&
        source.interpolation != UsdGeomTokens->varying) {
        source.error = primvar + " is " + source.interpolation.GetString() + ": one value for the " +
                       (source.interpolation == UsdGeomTokens->uniform ? "face" : "mesh") + ", nothing to lay out.";
        return source;
    }
    if (!convert<GfVec2f>(value, source.values) && !convert<GfVec2d>(value, source.values) && !convert<GfVec2h>(value, source.values)) {
        source.error = primvar + " isn't two numbers a value.";
        return source;
    }
    uv.GetIndices(&source.indices, time);
    mesh.GetFaceVertexCountsAttr().Get(&source.faceVertexCounts, time);
    mesh.GetFaceVertexIndicesAttr().Get(&source.faceVertexIndices, time);
    return source;
}

UvLayout computeUvLayout(const UvSource& source) {
    UvLayout layout;
    layout.prim = source.prim;
    layout.primvar = source.primvar;
    layout.interpolation = source.interpolation;
    layout.error = source.error;
    if (!layout.error.empty()) return layout;

    const bool perPoint = source.interpolation != UsdGeomTokens->faceVarying;
    const std::size_t cornerCount = source.faceVertexIndices.size();
    const std::size_t indexed = source.indices.empty() ? 0 : source.indices.size();
    // How many UVs there are, through the indices if there are any.
    const std::size_t uvCount = indexed ? indexed : source.values.size();
    const auto uvAt = [&](std::size_t i, bool* bad) -> GfVec2f {
        std::size_t at = i;
        if (indexed) {
            if (i >= indexed || source.indices[i] < 0 || std::size_t(source.indices[i]) >= source.values.size()) {
                *bad = true;
                return GfVec2f(0);
            }
            at = std::size_t(source.indices[i]);
        }
        if (at >= source.values.size()) {
            *bad = true;
            return GfVec2f(0);
        }
        return source.values[at];
    };
    if (!perPoint && uvCount != cornerCount) {
        layout.error = source.primvar + " has " + std::to_string(uvCount) + " UVs for " + std::to_string(cornerCount) + " face corners.";
        return layout;
    }

    layout.corners.resize(cornerCount);
    layout.cornerPoints.assign(source.faceVertexIndices.cbegin(), source.faceVertexIndices.cend());
    for (std::size_t c = 0; c < cornerCount; ++c) {
        bool bad = false;
        const int point = source.faceVertexIndices[c];
        layout.corners[c] = perPoint ? (point >= 0 ? uvAt(std::size_t(point), &bad) : (bad = true, GfVec2f(0))) : uvAt(c, &bad);
        if (bad) ++layout.badIndices;
        layout.bounds.UnionWith(layout.corners[c]);
    }

    // Faces: where they start, their area's sign, and how they sit.
    std::vector<double> areas;
    std::size_t start = 0;
    for (const int count : source.faceVertexCounts) {
        const int n = std::max(count, 0);
        if (start + std::size_t(n) > cornerCount) break;
        layout.faceStarts.push_back(start);
        layout.faceCounts.push_back(n);
        areas.push_back(n >= 3 ? signedArea(layout.corners, start, n) : 0.0);
        start += std::size_t(n);
    }
    const std::size_t faces = layout.faceStarts.size();
    layout.flags.assign(faces, 0);
    double total = 0;
    std::size_t positive = 0, negative = 0;
    for (const double area : areas) {
        total += std::abs(area);
        positive += area > 0;
        negative += area < 0;
    }
    // Flipped is against the way most faces go; degenerate is next to nothing beside the mean.
    const bool mostlyNegative = negative > positive;
    const double tiny = faces ? total / double(faces) * 1e-6 : 0.0;
    std::set<int> tiles;
    for (std::size_t f = 0; f < faces; ++f) {
        std::uint8_t& flag = layout.flags[f];
        if (layout.faceCounts[f] < 3 || std::abs(areas[f]) <= tiny) {
            flag |= UvLayout::Degenerate;
            ++layout.degenerate;
        } else if ((areas[f] < 0) != mostlyNegative) {
            flag |= UvLayout::Flipped;
            ++layout.flipped;
        }
        // A face is in the tile its middle is in: one from 0 to 1 is in 1001 only.
        GfVec2f middle(0);
        for (int i = 0; i < layout.faceCounts[f]; ++i) {
            const GfVec2f& uv = layout.corner(f, i);
            if (uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1) flag |= UvLayout::Outside;
            middle += uv;
        }
        if (layout.faceCounts[f] > 0) {
            if (const int tile = udimTile(middle / float(layout.faceCounts[f]))) tiles.insert(tile);
        }
        if (flag & UvLayout::Outside) ++layout.outside;
    }
    layout.udimTiles.assign(tiles.begin(), tiles.end());

    // Seams: an edge of the mesh two faces share, with other UVs on each side.
    std::unordered_map<std::uint64_t, std::pair<std::size_t, std::size_t>> edges;  // points -> its first corner pair
    edges.reserve(cornerCount);
    for (std::size_t f = 0; f < faces; ++f) {
        const int n = layout.faceCounts[f];
        for (int i = 0; i < n; ++i) {
            const std::size_t a = layout.faceStarts[f] + std::size_t(i);
            const std::size_t b = layout.faceStarts[f] + std::size_t((i + 1) % n);
            const int pa = layout.cornerPoints[a], pb = layout.cornerPoints[b];
            if (pa < 0 || pb < 0 || pa == pb) continue;
            const std::uint64_t key = (std::uint64_t(std::uint32_t(std::min(pa, pb))) << 32) | std::uint32_t(std::max(pa, pb));
            const auto [it, first] = edges.try_emplace(key, a, b);
            if (first) continue;
            // The other side's corners, matched by point.
            const auto [oa, ob] = it->second;
            const std::size_t sameA = layout.cornerPoints[oa] == pa ? oa : ob;
            const std::size_t sameB = sameA == oa ? ob : oa;
            if (layout.corners[sameA] != layout.corners[a] || layout.corners[sameB] != layout.corners[b]) {
                layout.seams.emplace_back(a, b);
                layout.seams.emplace_back(oa, ob);
            }
        }
    }

    // A grid to find faces by: about four faces a cell, up to 256 cells a side.
    if (faces > 0 && !layout.bounds.IsEmpty()) {
        layout.gridSize = std::clamp(int(std::sqrt(double(faces) / 4.0)), 1, 256);
        layout.grid.resize(std::size_t(layout.gridSize) * std::size_t(layout.gridSize));
        const GfVec2f low = layout.bounds.GetMin(), size = layout.bounds.GetSize();
        const auto cell = [&](float v, int axis) {
            const float extent = size[axis] > 0 ? size[axis] : 1.0f;
            return std::clamp(int((v - low[axis]) / extent * float(layout.gridSize)), 0, layout.gridSize - 1);
        };
        for (std::size_t f = 0; f < faces; ++f) {
            GfRange2f faceBounds;
            for (int i = 0; i < layout.faceCounts[f]; ++i) faceBounds.UnionWith(layout.corner(f, i));
            if (faceBounds.IsEmpty()) continue;
            for (int y = cell(faceBounds.GetMin()[1], 1); y <= cell(faceBounds.GetMax()[1], 1); ++y) {
                for (int x = cell(faceBounds.GetMin()[0], 0); x <= cell(faceBounds.GetMax()[0], 0); ++x) {
                    layout.grid[std::size_t(y) * std::size_t(layout.gridSize) + std::size_t(x)].push_back(std::uint32_t(f));
                }
            }
        }
    }
    return layout;
}

std::optional<std::size_t> UvLayout::faceAt(const GfVec2f& uv) const {
    if (gridSize == 0 || !bounds.Contains(uv)) return std::nullopt;
    const GfVec2f low = bounds.GetMin(), size = bounds.GetSize();
    const auto cell = [&](float v, int axis) {
        const float extent = size[axis] > 0 ? size[axis] : 1.0f;
        return std::clamp(int((v - low[axis]) / extent * float(gridSize)), 0, gridSize - 1);
    };
    std::optional<std::size_t> found;
    for (const std::uint32_t f : grid[std::size_t(cell(uv[1], 1)) * std::size_t(gridSize) + std::size_t(cell(uv[0], 0))]) {
        if (faceCounts[f] >= 3 && inside(uv, corners, faceStarts[f], faceCounts[f])) found = f;
    }
    return found;
}

std::vector<UvTexture> texturesReading(const UsdPrim& prim, const std::string& primvar) {
    std::vector<UvTexture> textures;
    const UsdShadeMaterial material = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    if (!material) return textures;
    for (const UsdPrim& node : UsdPrimRange(material.GetPrim())) {
        const UsdShadeShader shader(node);
        const UsdShadeInput file = shader ? shader.GetInput(TfToken("file")) : UsdShadeInput();
        if (!file || setRead(shader) != primvar) continue;
        const UsdShadeAttributeVector values = file.GetValueProducingAttributes();
        SdfAssetPath asset;
        if (values.empty() || !values.front().Get(&asset) || asset.GetAssetPath().empty()) continue;
        std::string path = asset.GetResolvedPath();
        // A UDIM path doesn't resolve as it is: anchor it to the layer it's written in.
        if (path.empty()) {
            for (const SdfPropertySpecHandle& spec : values.front().GetPropertyStack()) {
                if (!spec || !spec->HasDefaultValue()) continue;
                path = anchorUdim(spec->GetLayer(), asset.GetAssetPath());
                break;
            }
        }
        textures.push_back({node.GetPath(), path.empty() ? asset.GetAssetPath() : path});
    }
    return textures;
}

}  // namespace usdcc::uv
