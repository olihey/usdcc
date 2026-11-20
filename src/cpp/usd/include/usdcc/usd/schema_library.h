#pragma once

#include "usdcc/usd/schema_info.h"

#include <pxr/base/vt/value.h>

#include <string>
#include <vector>

namespace usdcc::usd {

// A codeless schema library (docs/SCHEMAS.md §5): its schema.usda as the
// Schema Editor sees it, read and written through Sdf, and the checks it makes
// before generating. Qt-free. Generating is schema_generator.h's.
//
// A library lives in a usd folder of a usdcc plugin (§5.1):
//   usd/plugInfo.json, usd/generatedSchema.usda   generated
//   usd/<name>/schema.usda                         its source

struct SchemaPropertyDef {
    std::string name;  // "studio:status"; a multiple-apply API's without its prefix ("value")
    bool relationship = false;
    std::string typeName;  // "token", "float3"; empty for a relationship
    bool uniform = false;
    std::string fallback;  // as USDA writes it ("wip" quoted, 1001, (0, 0, 0)); empty for none
    std::vector<std::string> allowedTokens;
    std::string documentation;
    std::string displayName;
    std::string displayGroup;
    bool operator==(const SchemaPropertyDef&) const = default;
};

struct SchemaClassDef {
    std::string name;  // "StudioAsset", "StudioShotAPI"
    // ConcreteTyped, AbstractTyped, SingleApplyAPI or MultipleApplyAPI.
    SchemaInfo::Kind kind = SchemaInfo::Kind::ConcreteTyped;
    std::string base;  // a typed schema's ("Xformable"); an API's is APISchemaBase
    std::string documentation;
    std::vector<std::string> builtInAPIs;     // prepend apiSchemas
    std::vector<std::string> autoApplyTo;     // an API's apiSchemaAutoApplyTo
    std::vector<std::string> canOnlyApplyTo;  // an API's apiSchemaCanOnlyApplyTo
    std::string propertyNamespacePrefix;      // a multiple-apply API's
    std::vector<SchemaPropertyDef> properties;  // by name, as USD writes them
    bool operator==(const SchemaClassDef&) const = default;
};

struct SchemaLibraryDef {
    std::string name;    // libraryName: "studio"; the plugin's name too
    std::string prefix;  // libraryPrefix, "" for usdGenSchema's default (the name, capitalized)
    // The schema.usda files it builds on: @usd/schema.usda@, @usdGeom/schema.usda@, ...
    std::vector<std::string> sublayers;
    std::vector<SchemaClassDef> classes;  // in the file's order
    bool operator==(const SchemaLibraryDef&) const = default;
};

// Where a library's files are in a usd folder.
std::string schemaSourcePath(const std::string& usdFolder, const std::string& library);
std::string generatedSchemaPath(const std::string& usdFolder);
std::string plugInfoPath(const std::string& usdFolder);

// Reads a schema.usda. False, with `error`, if it can't be read or isn't one
// (no GLOBAL prim with a libraryName).
bool readSchemaLibrary(const std::string& path, SchemaLibraryDef* library, std::string* error);
// Writes `library` to `path` through Sdf: an existing file is updated, so what
// the definition doesn't cover (other metadata, customData, a class's own
// fields) is kept. Classes and properties it doesn't list are removed. A
// codeless library always gets skipCodeGeneration = true. False, with
// `error`, if a fallback doesn't parse or the file can't be written.
bool writeSchemaLibrary(const SchemaLibraryDef& library, const std::string& path, std::string* error);

// A fallback's USDA text as a value of `typeName` ("int", "token[]"); false,
// with USD's own message in `error`, if it doesn't parse as one.
bool parseFallback(const std::string& typeName, const std::string& text, PXR_NS::VtValue* value, std::string* error);
// A value as USDA writes it: strings and tokens quoted, assets in @s, true and
// false, arrays of them too.
std::string usdaValueText(const PXR_NS::VtValue& value);

// What's wrong with a library before generating it (docs/SCHEMAS.md §5.4). An
// error stops Generate; a warning is said and can be ignored.
struct SchemaIssue {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string schema;    // the class it's about, or empty for the library
    std::string property;  // the property, or empty
    std::string message;
};
// `bases` are the libraries it builds on, not registered yet: their classes
// count as known.
std::vector<SchemaIssue> checkSchemaLibrary(const SchemaLibraryDef& library,
                                            const std::vector<SchemaLibraryDef>& bases = {});
bool hasErrors(const std::vector<SchemaIssue>& issues);
// "StudioAsset.studio:status: ..." for a log or a message box.
std::string issueText(const SchemaIssue& issue);

// The library in a usd folder: the one <name>/schema.usda in it. False, with
// `error`, if there's none or it can't be read.
bool findSchemaLibrary(const std::string& usdFolder, SchemaLibraryDef* library, std::string* error);

// usdGenSchema's TfType prefix for a library: its libraryPrefix, else its
// name with the first letter capitalized.
std::string libraryTypePrefix(const SchemaLibraryDef& library);

// Fills in the three @PLUG_INFO_...@ placeholders usdGenSchema leaves in a
// plugInfo.json for OpenUSD's CMake (docs/SCHEMAS.md §2, trap 2): no library,
// resources and root here. False, with `error`, if it can't be read or written.
bool fillPlugInfoPlaceholders(const std::string& path, std::string* error);

}  // namespace usdcc::usd
