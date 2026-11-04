#include "usdcc/usd/value_type_info.h"

// GfDuration (the "duration" value type) isn't in every OpenUSD build usdcc
// is built against; without it, that type is simply not there.
#if __has_include(<pxr/base/gf/duration.h>)
#include <pxr/base/gf/duration.h>
#define USDCC_HAS_GF_DURATION 1
#else
#define USDCC_HAS_GF_DURATION 0
#endif
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/matrix2d.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/timeCode.h>
#include <pxr/base/gf/traits.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/pathExpression.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/types.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <type_traits>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {

// Every element type a value (or array element) can have, for the
// type-dispatching helpers below. SdfOpaqueValue (opaque/group) is left out
// on purpose: no value of it can be authored.
template <class... Ts>
struct TypeList {};
#if USDCC_HAS_GF_DURATION
using ElementTypes =
    TypeList<bool, unsigned char, int, unsigned int, int64_t, uint64_t, GfHalf, float, double, GfTimeCode, GfDuration,
             std::string, TfToken, SdfAssetPath, SdfPathExpression, GfVec2i, GfVec3i, GfVec4i, GfVec2h, GfVec3h,
             GfVec4h, GfVec2f, GfVec3f, GfVec4f, GfVec2d, GfVec3d, GfVec4d, GfQuath, GfQuatf, GfQuatd, GfMatrix2d,
             GfMatrix3d, GfMatrix4d>;
template <class T>
constexpr bool kIsDuration = std::is_same_v<T, GfDuration>;
#else
using ElementTypes =
    TypeList<bool, unsigned char, int, unsigned int, int64_t, uint64_t, GfHalf, float, double, GfTimeCode,
             std::string, TfToken, SdfAssetPath, SdfPathExpression, GfVec2i, GfVec3i, GfVec4i, GfVec2h, GfVec3h,
             GfVec4h, GfVec2f, GfVec3f, GfVec4f, GfVec2d, GfVec3d, GfVec4d, GfQuath, GfQuatf, GfQuatd, GfMatrix2d,
             GfMatrix3d, GfMatrix4d>;
template <class T>
constexpr bool kIsDuration = false;
#endif

// Calls f.template operator()<T>() for the T whose TfType is `type`.
template <class F, class... Ts>
bool visitType(const TfType& type, F&& f, TypeList<Ts...>) {
    return ((type == TfType::Find<Ts>() ? (f.template operator()<Ts>(), true) : false) || ...);
}
template <class F>
bool visitType(const TfType& type, F&& f) {
    return visitType(type, std::forward<F>(f), ElementTypes{});
}

// Calls f.template operator()<T>() for the T such that `value` holds a VtArray<T>.
template <class F, class... Ts>
bool visitArray(const VtValue& value, F&& f, TypeList<Ts...>) {
    return ((value.IsHolding<VtArray<Ts>>() ? (f.template operator()<Ts>(), true) : false) || ...);
}
template <class F>
bool visitArray(const VtValue& value, F&& f) {
    return visitArray(value, std::forward<F>(f), ElementTypes{});
}

template <class T>
constexpr bool kIsNumber = (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) || std::is_same_v<T, GfHalf> ||
                           std::is_same_v<T, GfTimeCode> || kIsDuration<T>;

template <class T>
double toDouble(const T& v) {
    if constexpr (std::is_same_v<T, GfHalf>) {
        return static_cast<float>(v);
    } else if constexpr (std::is_same_v<T, GfTimeCode> || kIsDuration<T>) {
        return v.GetValue();
    } else {
        return static_cast<double>(v);
    }
}

template <class T>
T fromDouble(double d) {
    if constexpr (std::is_same_v<T, GfHalf>) {
        return GfHalf(static_cast<float>(d));
    } else if constexpr (std::is_same_v<T, GfTimeCode> || kIsDuration<T>) {
        return T(d);
    } else if constexpr (std::is_integral_v<T>) {
        const double lo = static_cast<double>(std::numeric_limits<T>::lowest());
        const double hi = static_cast<double>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp(std::round(d), lo, hi));
    } else {
        return static_cast<T>(d);
    }
}

template <class T>
std::vector<double> componentsOf(const T& v) {
    std::vector<double> out;
    if constexpr (kIsNumber<T>) {
        out.push_back(toDouble(v));
    } else if constexpr (GfIsGfVec<T>::value) {
        for (size_t i = 0; i < T::dimension; ++i) {
            out.push_back(toDouble(v[i]));
        }
    } else if constexpr (GfIsGfQuat<T>::value) {
        out.push_back(toDouble(v.GetReal()));
        for (size_t i = 0; i < 3; ++i) {
            out.push_back(toDouble(v.GetImaginary()[i]));
        }
    } else if constexpr (GfIsGfMatrix<T>::value) {
        for (size_t r = 0; r < T::numRows; ++r) {
            for (size_t c = 0; c < T::numColumns; ++c) {
                out.push_back(v[r][c]);
            }
        }
    }
    return out;
}

template <class T>
bool fromComponents(const std::vector<double>& n, T* out) {
    if constexpr (kIsNumber<T>) {
        if (n.size() != 1) return false;
        *out = fromDouble<T>(n[0]);
        return true;
    } else if constexpr (GfIsGfVec<T>::value) {
        if (n.size() != T::dimension) return false;
        for (size_t i = 0; i < T::dimension; ++i) {
            (*out)[i] = fromDouble<typename T::ScalarType>(n[i]);
        }
        return true;
    } else if constexpr (GfIsGfQuat<T>::value) {
        if (n.size() != 4) return false;
        using S = typename T::ScalarType;
        *out = T(fromDouble<S>(n[0]), fromDouble<S>(n[1]), fromDouble<S>(n[2]), fromDouble<S>(n[3]));
        return true;
    } else if constexpr (GfIsGfMatrix<T>::value) {
        if (n.size() != T::numRows * T::numColumns) return false;
        for (size_t r = 0; r < T::numRows; ++r) {
            for (size_t c = 0; c < T::numColumns; ++c) {
                (*out)[r][c] = n[r * T::numColumns + c];
            }
        }
        return true;
    } else {
        return false;
    }
}

std::string formatNumber(double v, int decimals, bool integer) {
    char buf[64];
    if (integer) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    // Trim trailing zeros but keep one decimal: 5.000 -> 5.0, 0.420 -> 0.42.
    if (const auto dot = s.find('.'); dot != std::string::npos) {
        while (s.size() > dot + 2 && s.back() == '0') {
            s.pop_back();
        }
    }
    if (s == "-0.0") {
        s = "0.0";
    }
    return s;
}

std::string formatScalar(const VtValue& value, int decimals) {
    const std::vector<double> numbers = numericComponents(value);
    if (!numbers.empty()) {
        const bool integer = value.IsHolding<int>() || value.IsHolding<unsigned int>() ||
                             value.IsHolding<int64_t>() || value.IsHolding<uint64_t>() ||
                             value.IsHolding<unsigned char>() || value.IsHolding<GfVec2i>() ||
                             value.IsHolding<GfVec3i>() || value.IsHolding<GfVec4i>();
        if (numbers.size() == 1) {
            return formatNumber(numbers[0], decimals, integer);
        }
        std::string s = "(";
        for (size_t i = 0; i < numbers.size(); ++i) {
            s += (i ? ", " : "") + formatNumber(numbers[i], decimals, integer);
        }
        return s + ")";
    }
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>() ? "true" : "false";
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<SdfAssetPath>()) return value.UncheckedGet<SdfAssetPath>().GetAssetPath();
    if (value.IsHolding<SdfPathExpression>()) return value.UncheckedGet<SdfPathExpression>().GetText();
    if (value.IsEmpty()) return {};
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

}  // namespace

bool ValueTypeInfo::isNumeric() const {
    switch (scalarKind) {
        case ScalarKind::UChar:
        case ScalarKind::Int:
        case ScalarKind::UInt:
        case ScalarKind::Int64:
        case ScalarKind::UInt64:
        case ScalarKind::Half:
        case ScalarKind::Float:
        case ScalarKind::Double:
        case ScalarKind::TimeCode:
        case ScalarKind::Duration:
            return true;
        default:
            return false;
    }
}

bool ValueTypeInfo::isInteger() const {
    switch (scalarKind) {
        case ScalarKind::UChar:
        case ScalarKind::Int:
        case ScalarKind::UInt:
        case ScalarKind::Int64:
        case ScalarKind::UInt64:
            return true;
        default:
            return false;
    }
}

int ValueTypeInfo::numberCount() const { return shape == ValueShape::Matrix ? components * components : components; }

std::vector<std::string> ValueTypeInfo::componentLabels() const {
    if (shape == ValueShape::Quaternion) {
        return {"W", "X", "Y", "Z"};
    }
    if (shape != ValueShape::Vector) {
        return {};
    }
    std::vector<std::string> labels;
    const char* names = role == SdfValueRoleNames->Color               ? "RGBA"
                        : role == SdfValueRoleNames->TextureCoordinate ? "UVW"
                                                                       : "XYZW";
    for (int i = 0; i < components; ++i) {
        labels.emplace_back(1, names[i]);
    }
    return labels;
}

ValueTypeInfo ValueTypeInfo::classify(const SdfValueTypeName& typeName) {
    ValueTypeInfo info;
    info.typeName = typeName;
    info.isArray = typeName.IsArray();
    info.scalarTypeName = info.isArray ? typeName.GetScalarType() : typeName;
    info.role = info.scalarTypeName.GetRole();
    const TfType type = info.scalarTypeName.GetType();

    auto kindOf = [](const TfType& t) {
        if (t == TfType::Find<bool>()) return ScalarKind::Bool;
        if (t == TfType::Find<unsigned char>()) return ScalarKind::UChar;
        if (t == TfType::Find<int>()) return ScalarKind::Int;
        if (t == TfType::Find<unsigned int>()) return ScalarKind::UInt;
        if (t == TfType::Find<int64_t>()) return ScalarKind::Int64;
        if (t == TfType::Find<uint64_t>()) return ScalarKind::UInt64;
        if (t == TfType::Find<GfHalf>()) return ScalarKind::Half;
        if (t == TfType::Find<float>()) return ScalarKind::Float;
        if (t == TfType::Find<double>()) return ScalarKind::Double;
        if (t == TfType::Find<GfTimeCode>()) return ScalarKind::TimeCode;
#if USDCC_HAS_GF_DURATION
        if (t == TfType::Find<GfDuration>()) return ScalarKind::Duration;
#endif
        if (t == TfType::Find<std::string>()) return ScalarKind::String;
        if (t == TfType::Find<TfToken>()) return ScalarKind::Token;
        if (t == TfType::Find<SdfAssetPath>()) return ScalarKind::Asset;
        if (t == TfType::Find<SdfPathExpression>()) return ScalarKind::PathExpression;
        if (t == TfType::Find<SdfOpaqueValue>()) return ScalarKind::Opaque;
        return ScalarKind::Unknown;
    };

    info.scalarKind = kindOf(type);
    if (info.scalarKind == ScalarKind::Unknown) {
        visitType(type, [&info, &kindOf]<class T>() {
            if constexpr (GfIsGfVec<T>::value) {
                info.shape = ValueShape::Vector;
                info.components = static_cast<int>(T::dimension);
                info.scalarKind = kindOf(TfType::Find<typename T::ScalarType>());
            } else if constexpr (GfIsGfQuat<T>::value) {
                info.shape = ValueShape::Quaternion;
                info.components = 4;
                info.scalarKind = kindOf(TfType::Find<typename T::ScalarType>());
            } else if constexpr (GfIsGfMatrix<T>::value) {
                info.shape = ValueShape::Matrix;
                info.components = static_cast<int>(T::numRows);
                info.scalarKind = kindOf(TfType::Find<typename T::ScalarType>());
            }
        });
    }

    const TfToken& role = info.role;
    if (role == SdfValueRoleNames->Color) {
        info.group = TypeGroup::Colors;
    } else if (role == SdfValueRoleNames->Point || role == SdfValueRoleNames->Normal ||
               role == SdfValueRoleNames->Vector) {
        info.group = TypeGroup::PointsNormalsVectors;
    } else if (role == SdfValueRoleNames->TextureCoordinate) {
        info.group = TypeGroup::TextureCoordinates;
    } else if (info.shape == ValueShape::Matrix) {
        info.group = TypeGroup::Matrices;
    } else if (info.shape == ValueShape::Quaternion) {
        info.group = TypeGroup::Quaternions;
    } else if (info.shape == ValueShape::Vector) {
        info.group = TypeGroup::Vectors;
    } else if (info.isNumeric() || info.scalarKind == ScalarKind::Bool) {
        info.group = TypeGroup::Scalars;
    } else if (info.scalarKind == ScalarKind::String || info.scalarKind == ScalarKind::Token ||
               info.scalarKind == ScalarKind::Asset || info.scalarKind == ScalarKind::PathExpression) {
        info.group = TypeGroup::StringsAndPaths;
    } else {
        info.group = TypeGroup::Other;
    }
    return info;
}

const char* typeGroupLabel(TypeGroup group) {
    switch (group) {
        case TypeGroup::Scalars:
            return "Scalars";
        case TypeGroup::Vectors:
            return "Vectors";
        case TypeGroup::PointsNormalsVectors:
            return "Points / Normals / Vectors";
        case TypeGroup::TextureCoordinates:
            return "Texture Coordinates";
        case TypeGroup::Colors:
            return "Colors";
        case TypeGroup::Quaternions:
            return "Quaternions";
        case TypeGroup::Matrices:
            return "Matrices";
        case TypeGroup::StringsAndPaths:
            return "Strings & Paths";
        case TypeGroup::Other:
            return "Other";
    }
    return "Other";
}

std::vector<SdfValueTypeName> canonicalValueTypes(bool includeArrays) {
    std::vector<SdfValueTypeName> types;
    for (const SdfValueTypeName& type : SdfSchema::GetInstance().GetAllTypes()) {
        const std::string& name = type.GetAsToken().GetString();
        if (name.empty() || !std::islower(static_cast<unsigned char>(name[0]))) {
            continue;  // legacy alias such as "Vec3f" or "PointFloat"
        }
        if (type.IsArray() && !includeArrays) {
            continue;
        }
        types.push_back(type);
    }
    std::sort(types.begin(), types.end(), [](const SdfValueTypeName& a, const SdfValueTypeName& b) {
        const ValueTypeInfo ia = ValueTypeInfo::classify(a);
        const ValueTypeInfo ib = ValueTypeInfo::classify(b);
        if (ia.isArray != ib.isArray) return !ia.isArray;
        if (ia.group != ib.group) return ia.group < ib.group;
        return a.GetAsToken().GetString() < b.GetAsToken().GetString();
    });
    return types;
}

std::vector<double> numericComponents(const VtValue& value) {
    std::vector<double> out;
    visitType(value.GetType(), [&]<class T>() {
        if constexpr (kIsNumber<T> || GfIsGfVec<T>::value || GfIsGfQuat<T>::value || GfIsGfMatrix<T>::value) {
            out = componentsOf(value.UncheckedGet<T>());
        }
    });
    return out;
}

VtValue makeNumericValue(const SdfValueTypeName& scalarType, const std::vector<double>& numbers) {
    VtValue result;
    visitType(scalarType.GetType(), [&]<class T>() {
        if constexpr (kIsNumber<T> || GfIsGfVec<T>::value || GfIsGfQuat<T>::value || GfIsGfMatrix<T>::value) {
            T v{};
            if (fromComponents(numbers, &v)) {
                result = VtValue(v);
            }
        }
    });
    return result;
}

std::size_t arraySize(const VtValue& array) { return array.IsArrayValued() ? array.GetArraySize() : 0; }

VtValue arrayElement(const VtValue& array, std::size_t index) {
    VtValue element;
    visitArray(array, [&]<class T>() {
        const auto& a = array.UncheckedGet<VtArray<T>>();
        if (index < a.size()) {
            element = VtValue(a[index]);
        }
    });
    return element;
}

bool setArrayElement(VtValue& array, std::size_t index, const VtValue& element) {
    bool ok = false;
    visitArray(array, [&]<class T>() {
        if (!element.IsHolding<T>()) return;
        VtArray<T> a = array.UncheckedGet<VtArray<T>>();
        if (index >= a.size()) return;
        a[index] = element.UncheckedGet<T>();
        array = VtValue(std::move(a));
        ok = true;
    });
    return ok;
}

bool insertArrayElement(VtValue& array, std::size_t index, const VtValue& element) {
    bool ok = false;
    visitArray(array, [&]<class T>() {
        if (!element.IsHolding<T>()) return;
        VtArray<T> a = array.UncheckedGet<VtArray<T>>();
        if (index > a.size()) return;
        a.insert(a.begin() + static_cast<std::ptrdiff_t>(index), element.UncheckedGet<T>());
        array = VtValue(std::move(a));
        ok = true;
    });
    return ok;
}

bool removeArrayElement(VtValue& array, std::size_t index) {
    bool ok = false;
    visitArray(array, [&]<class T>() {
        VtArray<T> a = array.UncheckedGet<VtArray<T>>();
        if (index >= a.size()) return;
        a.erase(a.begin() + static_cast<std::ptrdiff_t>(index));
        array = VtValue(std::move(a));
        ok = true;
    });
    return ok;
}

bool resizeArray(VtValue& array, std::size_t size) {
    bool ok = false;
    visitArray(array, [&]<class T>() {
        VtArray<T> a = array.UncheckedGet<VtArray<T>>();
        a.resize(size);
        array = VtValue(std::move(a));
        ok = true;
    });
    return ok;
}

VtValue emptyArray(const SdfValueTypeName& arrayType) {
    VtValue result;
    if (!arrayType.IsArray()) return result;
    visitType(arrayType.GetScalarType().GetType(), [&]<class T>() { result = VtValue(VtArray<T>()); });
    return result;
}

VtValue sampleValue(const SdfValueTypeName& typeName) {
    const ValueTypeInfo info = ValueTypeInfo::classify(typeName);

    // The i-th sample element, so array elements differ from each other.
    auto sampleScalar = [&info](int i) -> VtValue {
        const SdfValueTypeName& t = info.scalarTypeName;
        const double k = 1.0 + i;
        if (info.isNumeric()) {
            std::vector<double> n;
            switch (info.shape) {
                case ValueShape::Scalar:
                    n = {info.isInteger() ? 7.0 * k
                         : info.scalarKind == ScalarKind::TimeCode ? 24.0 * k
                         : info.scalarKind == ScalarKind::Duration ? 1.5 * k
                                                                   : 0.42 * k};
                    break;
                case ValueShape::Vector:
                    if (info.role == SdfValueRoleNames->Color) {
                        n = {0.24, 0.55, 0.99, 1.0};
                        n[0] = std::fmod(0.24 * k, 1.0);
                    } else if (info.role == SdfValueRoleNames->Normal) {
                        n = {0.0, 1.0, 0.0};
                    } else {
                        n = {1.0 * k, 2.0 * k, 3.0 * k, 4.0 * k};
                    }
                    n.resize(static_cast<size_t>(info.components));
                    break;
                case ValueShape::Quaternion:
                    // 45 degrees about Y.
                    n = {0.92388, 0.0, 0.38268, 0.0};
                    break;
                case ValueShape::Matrix:
                    for (int r = 0; r < info.components; ++r) {
                        for (int c = 0; c < info.components; ++c) {
                            n.push_back(r == c ? 1.0 : 0.0);
                        }
                    }
                    if (info.components == 4) {
                        n[12] = 1.0 * k;
                        n[13] = 2.0;
                        n[14] = 3.0;
                    }
                    break;
            }
            return makeNumericValue(t, n);
        }
        switch (info.scalarKind) {
            case ScalarKind::Bool:
                return VtValue(i % 2 == 0);
            case ScalarKind::String:
                return VtValue(std::string(i == 0 ? "Hello, USD" : "item ") + (i ? std::to_string(i) : ""));
            case ScalarKind::Token:
                return VtValue(TfToken(i == 0 ? "default" : "token" + std::to_string(i)));
            case ScalarKind::Asset:
                return VtValue(SdfAssetPath(i == 0 ? "./textures/clay_albedo.exr"
                                                   : "./textures/tex" + std::to_string(i) + ".exr"));
            case ScalarKind::PathExpression:
                return VtValue(SdfPathExpression("/World/**"));
            default:
                return VtValue();
        }
    };

    if (!info.isArray) {
        return sampleScalar(0);
    }
    VtValue array = emptyArray(typeName);
    for (int i = 0; i < 4; ++i) {
        const VtValue element = sampleScalar(i);
        if (element.IsEmpty() || !insertArrayElement(array, arraySize(array), element)) {
            break;
        }
    }
    return array;
}

std::string formatValue(const VtValue& value, int decimals, std::size_t maxArrayElements) {
    if (!value.IsArrayValued()) {
        return formatScalar(value, decimals);
    }
    const std::size_t n = arraySize(value);
    std::string s = "[" + std::to_string(n) + "]";
    for (std::size_t i = 0; i < n && i < maxArrayElements; ++i) {
        s += (i ? ", " : " ") + formatScalar(arrayElement(value, i), decimals);
    }
    if (n > maxArrayElements) {
        s += ", \xE2\x80\xA6";  // UTF-8 ellipsis
    }
    return s;
}

}  // namespace usdcc::usd
