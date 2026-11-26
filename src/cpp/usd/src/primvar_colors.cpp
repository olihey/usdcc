#include "usdcc/usd/primvar_colors.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <type_traits>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace {
// --- Reading numbers out of any array -------------------------------------------------

double asNumber(float v) { return v; }
double asNumber(double v) { return v; }
double asNumber(GfHalf v) { return float(v); }
double asNumber(int v) { return v; }
double asNumber(unsigned int v) { return v; }
double asNumber(int64_t v) { return double(v); }
double asNumber(uint64_t v) { return double(v); }
double asNumber(unsigned char v) { return v; }
double asNumber(bool v) { return v ? 1.0 : 0.0; }

template <typename T>
struct Parts {
    static constexpr int size = 1;
    static double at(const T& value, int) { return asNumber(value); }
};
template <typename T>
    requires requires { T::dimension; }
struct Parts<T> {
    static constexpr int size = T::dimension;
    static double at(const T& value, int i) { return asNumber(value[i]); }
};

// Calls `each(numbers, size)` for each value of a number or vector array (or scalar); false for
// another type.
template <typename T, typename Each>
bool forNumbers(const VtValue& value, const Each& each) {
    const auto one = [&each](const T& v) {
        double parts[Parts<T>::size];
        for (int i = 0; i < Parts<T>::size; ++i) parts[i] = Parts<T>::at(v, i);
        each(parts, Parts<T>::size);
    };
    if (value.IsHolding<VtArray<T>>()) {
        for (const T& v : value.UncheckedGet<VtArray<T>>()) one(v);
        return true;
    }
    if (value.IsHolding<T>()) {
        one(value.UncheckedGet<T>());
        return true;
    }
    return false;
}

template <typename Each>
bool forEachNumber(const VtValue& value, const Each& each) {
    return forNumbers<float>(value, each) || forNumbers<double>(value, each) || forNumbers<GfHalf>(value, each) ||
           forNumbers<int>(value, each) || forNumbers<unsigned int>(value, each) || forNumbers<int64_t>(value, each) ||
           forNumbers<uint64_t>(value, each) || forNumbers<unsigned char>(value, each) || forNumbers<bool>(value, each) ||
           forNumbers<GfVec2f>(value, each) || forNumbers<GfVec3f>(value, each) || forNumbers<GfVec4f>(value, each) ||
           forNumbers<GfVec2d>(value, each) || forNumbers<GfVec3d>(value, each) || forNumbers<GfVec4d>(value, each) ||
           forNumbers<GfVec2h>(value, each) || forNumbers<GfVec3h>(value, each) || forNumbers<GfVec4h>(value, each) ||
           forNumbers<GfVec2i>(value, each) || forNumbers<GfVec3i>(value, each) || forNumbers<GfVec4i>(value, each);
}

// Calls `each(text)` for each value of whole numbers, bools, strings or tokens; false otherwise.
template <typename T, typename Each>
bool forWords(const VtValue& value, const Each& each) {
    const auto text = [](const T& v) {
        if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else if constexpr (std::is_same_v<T, TfToken>) {
            return v.GetString();
        } else {
            return std::to_string(static_cast<long long>(v));
        }
    };
    if (value.IsHolding<VtArray<T>>()) {
        for (const T& v : value.UncheckedGet<VtArray<T>>()) each(text(v));
        return true;
    }
    if (value.IsHolding<T>()) {
        each(text(value.UncheckedGet<T>()));
        return true;
    }
    return false;
}

template <typename Each>
bool forEachWord(const VtValue& value, const Each& each) {
    return forWords<int>(value, each) || forWords<unsigned int>(value, each) || forWords<int64_t>(value, each) ||
           forWords<uint64_t>(value, each) || forWords<unsigned char>(value, each) || forWords<bool>(value, each) ||
           forWords<std::string>(value, each) || forWords<TfToken>(value, each);
}

// What the ramp maps of one value: a component, or the length.
double rampNumber(const double* parts, int size, int component) {
    if (size == 1) return parts[0];
    if (component >= 0) return parts[std::min(component, size - 1)];
    double sum = 0.0;
    for (int i = 0; i < size; ++i) sum += parts[i] * parts[i];
    return std::sqrt(sum);
}

std::size_t valueCount(const VtValue& value) { return value.IsArrayValued() ? value.GetArraySize() : 1; }

bool scalarIs(const SdfValueTypeName& type, std::initializer_list<SdfValueTypeName> names) {
    const SdfValueTypeName scalar = type.GetScalarType();
    return std::any_of(names.begin(), names.end(), [&scalar](const SdfValueTypeName& name) { return scalar == name.GetScalarType(); });
}

bool isWords(const SdfValueTypeName& type) {
    const auto& n = *SdfValueTypeNames;
    return scalarIs(type, {n.Int, n.UInt, n.Int64, n.UInt64, n.UChar, n.Bool, n.String, n.Token});
}

int dimension(const SdfValueTypeName& type) {
    const SdfTupleDimensions dims = type.GetDimensions();
    return dims.size == 1 ? int(dims.d[0]) : dims.size == 0 ? 1 : 0;
}

// By the C++ type, so every role of it counts: point3f, color3f and float3 are all GfVec3f.
bool isNumbers(const SdfValueTypeName& type) {
    static const std::set<TfType> types = {
        TfType::Find<float>(),   TfType::Find<double>(),   TfType::Find<GfHalf>(),  TfType::Find<int>(),
        TfType::Find<unsigned int>(), TfType::Find<int64_t>(), TfType::Find<uint64_t>(), TfType::Find<unsigned char>(),
        TfType::Find<bool>(),    TfType::Find<GfVec2f>(),  TfType::Find<GfVec3f>(), TfType::Find<GfVec4f>(),
        TfType::Find<GfVec2d>(), TfType::Find<GfVec3d>(),  TfType::Find<GfVec4d>(), TfType::Find<GfVec2h>(),
        TfType::Find<GfVec3h>(), TfType::Find<GfVec4h>(),  TfType::Find<GfVec2i>(), TfType::Find<GfVec3i>(),
        TfType::Find<GfVec4i>()};
    return types.count(type.GetScalarType().GetType()) > 0;
}

std::vector<PrimvarColorizer> builtIn() {
    std::vector<PrimvarColorizer> result;
    // Ramp: numbers, a component or the length, over the range.
    result.push_back({"ramp", "Ramp", isNumbers,
                      [](const VtValue& values, const PrimvarColorizerInput& input) {
                          VtVec3fArray colors;
                          colors.reserve(valueCount(values));
                          const double span = input.range.high > input.range.low ? input.range.high - input.range.low : 1.0;
                          forEachNumber(values, [&](const double* parts, int size) {
                              const double n = rampNumber(parts, size, input.component);
                              colors.push_back(std::isfinite(n) ? primvarRampColor((n - input.range.low) / span) : GfVec3f(1, 0, 1));
                          });
                          return colors;
                      },
                      PrimvarColorizer::Legend::Ramp, {}});
    result.push_back({"categories", "Categories", isWords,
                      [](const VtValue& values, const PrimvarColorizerInput&) {
                          VtVec3fArray colors;
                          colors.reserve(valueCount(values));
                          forEachWord(values, [&colors](const std::string& text) { colors.push_back(primvarCategoryColor(text)); });
                          return colors;
                      },
                      PrimvarColorizer::Legend::Categories, {}});
    result.push_back({"color", "Color",
                      [](const SdfValueTypeName& type) { return isNumbers(type) && !isWords(type) && dimension(type) >= 3; },
                      [](const VtValue& values, const PrimvarColorizerInput&) {
                          VtVec3fArray colors;
                          colors.reserve(valueCount(values));
                          forEachNumber(values, [&colors](const double* parts, int size) {
                              GfVec3f color(0.0f);
                              for (int i = 0; i < std::min(size, 3); ++i) color[i] = float(std::clamp(parts[i], 0.0, 1.0));
                              colors.push_back(color);
                          });
                          return colors;
                      },
                      PrimvarColorizer::Legend::None, "the values as colours"});
    result.push_back({"uv", "UV", [](const SdfValueTypeName& type) { return isNumbers(type) && !isWords(type) && dimension(type) == 2; },
                      [](const VtValue& values, const PrimvarColorizerInput&) {
                          VtVec3fArray colors;
                          colors.reserve(valueCount(values));
                          forEachNumber(values, [&colors](const double* parts, int) {
                              colors.push_back(GfVec3f(float(std::clamp(parts[0], 0.0, 1.0)), float(std::clamp(parts[1], 0.0, 1.0)), 0.0f));
                          });
                          return colors;
                      },
                      PrimvarColorizer::Legend::None, "U red, V green"});
    result.push_back({"direction", "Direction",
                      [](const SdfValueTypeName& type) { return isNumbers(type) && !isWords(type) && dimension(type) == 3; },
                      [](const VtValue& values, const PrimvarColorizerInput&) {
                          VtVec3fArray colors;
                          colors.reserve(valueCount(values));
                          forEachNumber(values, [&colors](const double* parts, int) {
                              GfVec3d v(parts[0], parts[1], parts[2]);
                              const double length = v.GetLength();
                              if (length > 0) v /= length;
                              colors.push_back(GfVec3f(v * 0.5 + GfVec3d(0.5)));
                          });
                          return colors;
                      },
                      PrimvarColorizer::Legend::None, "x red, y green, z blue"});
    return result;
}
}  // namespace

// --- Colours ---------------------------------------------------------------------------

const std::vector<GfVec3f>& primvarRampStops() {
    static const std::vector<GfVec3f> stops{{0.05f, 0.1f, 0.6f}, {0.0f, 0.7f, 0.8f}, {0.1f, 0.75f, 0.2f}, {0.95f, 0.85f, 0.1f},
                                            {0.85f, 0.1f, 0.1f}};
    return stops;
}

GfVec3f primvarRampColor(double t) {
    const std::vector<GfVec3f>& stops = primvarRampStops();
    const double scaled = std::clamp(std::isfinite(t) ? t : 0.0, 0.0, 1.0) * double(stops.size() - 1);
    const std::size_t i = std::min(std::size_t(scaled), stops.size() - 2);
    const float f = float(scaled - double(i));
    return stops[i] * (1.0f - f) + stops[i + 1] * f;
}

GfVec3f primvarCategoryColor(const std::string& value) {
    // FNV-1a, then spread: the same text is the same colour in every session.
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : value) h = (h ^ c) * 16777619u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const auto channel = [](std::uint32_t bits) { return 0.25f + 0.75f * float(bits & 0xff) / 255.0f; };
    return {channel(h), channel(h >> 8), channel(h >> 16)};
}

GfVec3f primvarMissingColor() { return GfVec3f(0.35f); }

// --- PrimvarColorizers -----------------------------------------------------------------

PrimvarColorizers& PrimvarColorizers::instance() {
    static PrimvarColorizers* const colorizers = new PrimvarColorizers;  // outlives every viewport
    return *colorizers;
}

PrimvarColorizers::PrimvarColorizers() : m_colorizers(builtIn()) {}

void PrimvarColorizers::add(PrimvarColorizer colorizer) {
    const auto it = std::find_if(m_colorizers.begin(), m_colorizers.end(),
                                 [&colorizer](const PrimvarColorizer& other) { return other.id == colorizer.id; });
    if (it != m_colorizers.end()) {
        *it = std::move(colorizer);
    } else {
        m_colorizers.push_back(std::move(colorizer));
    }
}

bool PrimvarColorizers::remove(const std::string& id) {
    return std::erase_if(m_colorizers, [&id](const PrimvarColorizer& colorizer) { return colorizer.id == id; }) > 0;
}

const PrimvarColorizer* PrimvarColorizers::find(const std::string& id) const {
    const auto it = std::find_if(m_colorizers.begin(), m_colorizers.end(), [&id](const PrimvarColorizer& colorizer) { return colorizer.id == id; });
    return it == m_colorizers.end() ? nullptr : &*it;
}

const PrimvarColorizer* PrimvarColorizers::choose(const std::string& id, const SdfValueTypeName& type) const {
    if (const PrimvarColorizer* own = find(id); own && own->applies && own->applies(type)) return own;
    const SdfValueTypeName scalar = type.GetScalarType();
    const TfToken& role = scalar.GetRole();
    const auto builtIn = [this, &type](const char* each) -> const PrimvarColorizer* {
        const PrimvarColorizer* found = find(each);
        return found && found->applies(type) ? found : nullptr;
    };
    const PrimvarColorizer* chosen = nullptr;
    if (role == SdfValueRoleNames->Color) chosen = builtIn("color");
    if (!chosen && (role == SdfValueRoleNames->TextureCoordinate || dimension(type) == 2)) chosen = builtIn("uv");
    if (!chosen && (role == SdfValueRoleNames->Normal || role == SdfValueRoleNames->Vector)) chosen = builtIn("direction");
    if (!chosen && isWords(type)) chosen = builtIn("categories");
    if (!chosen) chosen = builtIn("ramp");
    return chosen;
}

// --- Stats -----------------------------------------------------------------------------

PrimvarColorStats primvarColorStats(const UsdStageRefPtr& stage, const std::string& name, UsdTimeCode time, int component) {
    PrimvarColorStats stats;
    if (!stage || name.empty()) return stats;
    const TfToken attributeName("primvars:" + name);
    double low = 0, high = 0;
    bool any = false;
    std::set<std::string> categories;
    for (const UsdPrim& prim : UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies(UsdPrimDefaultPredicate))) {
        const UsdAttribute attribute = prim.GetAttribute(attributeName);
        VtValue value;
        if (!attribute || !attribute.Get(&value, time) || value.IsEmpty()) continue;
        if (!stats.type) stats.type = attribute.GetTypeName();
        ++stats.prims;
        stats.timeVarying = stats.timeVarying || attribute.ValueMightBeTimeVarying();
        forEachNumber(value, [&](const double* parts, int size) {
            const double n = rampNumber(parts, size, component);
            if (!std::isfinite(n)) return;
            low = any ? std::min(low, n) : n;
            high = any ? std::max(high, n) : n;
            any = true;
        });
        if (!stats.moreCategories) {
            forEachWord(value, [&](const std::string& text) {
                if (categories.size() < PrimvarColorStats::kMaxCategories || categories.count(text)) {
                    categories.insert(text);
                } else {
                    stats.moreCategories = true;
                }
            });
        }
    }
    if (any) stats.range = PrimvarColorRange{low, high};
    stats.categories.assign(categories.begin(), categories.end());
    return stats;
}

}  // namespace usdcc::usd
