#pragma once

// A UV set's layout and what's wrong with it (docs/PRIMVAR_VIEWER.md §6, §18): each face's
// corners in UV space, from a mesh's topology and the primvar's values; faces that are flipped,
// degenerate or outside 0-1, the seams, the UDIM tiles it uses; and a grid to find the face under
// a point. Read on the main thread (readUvSource()), worked out on any (computeUvLayout()).
// Qt-free.

#include <pxr/base/gf/range2f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usdcc::uv {

// What a layout is made of, as it's read from the stage: owned data only.
struct UvSource {
    PXR_NS::SdfPath prim;
    std::string primvar;  // without "primvars:"
    PXR_NS::TfToken interpolation;
    PXR_NS::VtVec2fArray values;  // whatever two-number type it is, as floats
    PXR_NS::VtIntArray indices;   // none if it isn't indexed
    PXR_NS::VtIntArray faceVertexCounts;
    PXR_NS::VtIntArray faceVertexIndices;
    std::string error;  // why there's nothing to lay out; empty otherwise
};
// The mesh's topology and the primvar's values at `time`. `error` says why not, for a prim that
// isn't a mesh, a primvar it hasn't, one that isn't two numbers, or one that's constant or uniform
// (a value per face, not per corner).
UvSource readUvSource(const PXR_NS::UsdPrim& prim, const std::string& primvar, PXR_NS::UsdTimeCode time);

struct UvLayout {
    enum Flag : std::uint8_t {
        Flipped = 1,     // wound the other way round from most faces
        Degenerate = 2,  // no area, or fewer than three corners
        Outside = 4,     // a corner outside 0-1 (for UDIMs, that's how it should be)
    };

    PXR_NS::SdfPath prim;
    std::string primvar;
    PXR_NS::TfToken interpolation;
    std::string error;  // why there's no layout; empty otherwise

    std::vector<PXR_NS::GfVec2f> corners;  // a UV for each face corner, in faceVertexIndices' order
    std::vector<std::size_t> faceStarts;   // each face's first corner; faces() of them
    std::vector<int> faceCounts;
    std::vector<int> cornerPoints;         // the point each corner is at
    std::vector<std::uint8_t> flags;       // Flag bits, a byte a face
    // The UV edges along a seam, as corner pairs: where two faces share an edge of the mesh but
    // not its UVs, the edge on each side.
    std::vector<std::pair<std::size_t, std::size_t>> seams;
    PXR_NS::GfRange2f bounds;
    std::vector<int> udimTiles;  // sorted: the tiles faces are in (udimTile() of their middles)
    std::size_t flipped = 0, degenerate = 0, outside = 0, badIndices = 0;

    std::size_t faces() const { return faceStarts.size(); }
    // The face whose polygon holds `uv`, the last one drawn where they overlap; nothing outside all.
    std::optional<std::size_t> faceAt(const PXR_NS::GfVec2f& uv) const;
    // Where a face's corners are, for drawing it.
    PXR_NS::GfVec2f corner(std::size_t face, int i) const { return corners[faceStarts[face] + std::size_t(i)]; }

    // A grid over the bounds, each cell listing the faces whose bounds touch it.
    int gridSize = 0;
    std::vector<std::vector<std::uint32_t>> grid;
};

UvLayout computeUvLayout(const UvSource& source);

// The UDIM tile a UV is in: 1001 for 0-1, 1002 to its right, 1011 above; 0 outside 0-10 in u or
// below 0 in v.
int udimTile(const PXR_NS::GfVec2f& uv);

// A texture a bound material reads with this UV set: a UsdUVTexture's or MaterialX image's file,
// whose coordinates come from a reader of the set, or the primary set when nothing's connected.
struct UvTexture {
    PXR_NS::SdfPath shader;
    std::string file;  // resolved where it can be; may hold <UDIM>
};
std::vector<UvTexture> texturesReading(const PXR_NS::UsdPrim& prim, const std::string& primvar);

}  // namespace usdcc::uv
