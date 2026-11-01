#pragma once

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <cstddef>
#include <string>
#include <vector>

namespace usdcc::usd {

// Qt-free description of a USD value type, so property editors (and tests, and
// later Python) can pick a presentation without each re-deriving it from the
// SdfValueTypeName — see docs/ATTRIBUTE_EDITOR.md §2 and §4.

// The C++ type of one component of a value.
enum class ScalarKind {
    Bool,
    UChar,
    Int,
    UInt,
    Int64,
    UInt64,
    Half,
    Float,
    Double,
    TimeCode,
    Duration,
    String,
    Token,
    Asset,
    PathExpression,
    Opaque,  // `opaque` and `group`: no value can be authored
    Unknown,
};

enum class ValueShape { Scalar, Vector, Quaternion, Matrix };

// How the Debug > USD Data Types gallery (and docs) group the types.
enum class TypeGroup {
    Scalars,
    Vectors,
    PointsNormalsVectors,
    TextureCoordinates,
    Colors,
    Quaternions,
    Matrices,
    StringsAndPaths,
    Other,
};

struct ValueTypeInfo {
    PXR_NS::SdfValueTypeName typeName;        // as classified: scalar or array
    PXR_NS::SdfValueTypeName scalarTypeName;  // the element type for arrays, else the same
    bool isArray = false;
    ScalarKind scalarKind = ScalarKind::Unknown;
    ValueShape shape = ValueShape::Scalar;
    // Vector length (2-4), 4 for quaternions, the row count for (square)
    // matrices, 1 for scalars.
    int components = 1;
    PXR_NS::TfToken role;  // SdfValueRoleNames: Point, Normal, Vector, Color, TextureCoordinate, Frame, Transform, Group
    TypeGroup group = TypeGroup::Other;

    bool isNumeric() const;  // every component is a number (incl. half, timecode, duration)
    bool isInteger() const;
    // Total number of numbers in one element: components, or rows*rows for matrices.
    int numberCount() const;
    // Per-component labels: X Y Z W, U V W (texture coordinates), R G B A
    // (colors), W X Y Z (quaternions: real part first); empty for scalars and matrices.
    std::vector<std::string> componentLabels() const;

    static ValueTypeInfo classify(const PXR_NS::SdfValueTypeName& typeName);
};

const char* typeGroupLabel(TypeGroup group);

// Every value type this USD build registers, minus the legacy capitalized
// aliases (`Vec3f`, `Color`, `PointFloat`, …) that SdfSchema::GetAllTypes()
// also returns. Sorted by group, then name; scalar types before arrays.
std::vector<PXR_NS::SdfValueTypeName> canonicalValueTypes(bool includeArrays = true);

// Numeric access, for scalar/vector/quaternion/matrix values of any
// precision. numericComponents() is empty for non-numeric values; quaternions
// come out real part first, matrices row by row.
std::vector<double> numericComponents(const PXR_NS::VtValue& value);
// Builds a value of `scalarType` (not an array) from numbers in the same
// layout numericComponents() produces. Empty VtValue if the count is wrong.
PXR_NS::VtValue makeNumericValue(const PXR_NS::SdfValueTypeName& scalarType, const std::vector<double>& numbers);

// Array access on a VtValue holding a VtArray of any supported element type.
// All of these fail (return 0 / an empty value / false) for anything else.
std::size_t arraySize(const PXR_NS::VtValue& array);
PXR_NS::VtValue arrayElement(const PXR_NS::VtValue& array, std::size_t index);
bool setArrayElement(PXR_NS::VtValue& array, std::size_t index, const PXR_NS::VtValue& element);
bool insertArrayElement(PXR_NS::VtValue& array, std::size_t index, const PXR_NS::VtValue& element);
bool removeArrayElement(PXR_NS::VtValue& array, std::size_t index);
bool resizeArray(PXR_NS::VtValue& array, std::size_t size);
// An empty array of `arrayType` (an SdfValueTypeName with IsArray()).
PXR_NS::VtValue emptyArray(const PXR_NS::SdfValueTypeName& arrayType);

// A representative, non-default value of the type (arrays get a few elements);
// empty for `opaque`/`group`. Used by the data-type gallery and tests.
PXR_NS::VtValue sampleValue(const PXR_NS::SdfValueTypeName& typeName);

// Short human-readable text for a value: numbers trimmed to `decimals`,
// arrays as "[N] a, b, c…" with at most `maxArrayElements` shown.
std::string formatValue(const PXR_NS::VtValue& value, int decimals = 3, std::size_t maxArrayElements = 3);

}  // namespace usdcc::usd
