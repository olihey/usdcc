#include "usdcc/usd/validation_templates.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>

#include <algorithm>
#include <regex>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd::validation {

namespace {
using Type = TemplateField::Type;

const TemplateField kTypes{"types", "Prim types", "Schemas, such as Mesh or MaterialBindingAPI; none: every prim",
                           Type::TextList, false};

const JsValue* valueOf(const JsObject& values, const std::string& key) {
    const auto it = values.find(key);
    return it == values.end() ? nullptr : &it->second;
}
std::string text(const JsObject& values, const std::string& key) {
    const JsValue* value = valueOf(values, key);
    return value && value->IsString() ? value->GetString() : std::string();
}
bool flag(const JsObject& values, const std::string& key) {
    const JsValue* value = valueOf(values, key);
    return value && value->IsBool() && value->GetBool();
}
std::optional<double> number(const JsObject& values, const std::string& key) {
    const JsValue* value = valueOf(values, key);
    if (!value) return std::nullopt;
    if (value->IsReal()) return value->GetReal();
    if (value->IsInt()) return double(value->GetInt64());
    return std::nullopt;
}
std::vector<std::string> list(const JsObject& values, const std::string& key) {
    const JsValue* value = valueOf(values, key);
    std::vector<std::string> result;
    if (value && value->IsArrayOf<std::string>()) result = value->GetArrayOf<std::string>();
    return result;
}

TfType schemaType(const std::string& name) {
    const TfType type = UsdSchemaRegistry::GetTypeFromName(TfToken(name));
    return type ? type : TfType::FindByName(name);
}

bool hasSchema(const UsdPrim& prim, const TfType& type) {
    if (prim.IsA(type)) return true;
    const TfToken name = UsdSchemaRegistry::GetSchemaTypeName(type);
    const TfTokenVector applied = prim.GetAppliedSchemas();
    return std::any_of(applied.begin(), applied.end(), [&name](const TfToken& schema) {
        // "CollectionAPI:lights" is a CollectionAPI.
        return schema == name || TfStringStartsWith(schema.GetString(), name.GetString() + ":");
    });
}

// A number, or what it reads as.
std::optional<double> asNumber(const VtValue& value) {
    if (value.IsHolding<bool>()) return std::nullopt;
    const VtValue cast = VtValue::Cast<double>(value);
    if (cast.IsHolding<double>()) return cast.UncheckedGet<double>();
    return std::nullopt;
}
std::string asText(const VtValue& value) {
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>() ? "true" : "false";
    return TfStringify(value);
}

// Python for a string, or a list of them.
std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n') {
            result += "\\n";
        } else if (c != '\r') {
            result += c;
        }
    }
    return result + "\"";
}
std::string quoted(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const std::string& value : values) result += (result.size() > 1 ? ", " : "") + quoted(value);
    return result + "]";
}
std::string numberText(std::optional<double> value) {
    if (!value) return "None";
    return *value == double(int64_t(*value)) ? std::to_string(int64_t(*value)) : TfStringify(*value);
}
}  // namespace

const std::vector<CheckTemplate>& checkTemplates() {
    static const std::vector<CheckTemplate> templates = {
        {"name_pattern", "Name pattern", "Prims' names match a pattern.",
         {kTypes, {"pattern", "Pattern", "A regular expression for the whole name: [a-z0-9_]+", Type::Text, true}}},
        {"required_property", "Required property", "Prims have an attribute or relationship.",
         {kTypes, {"property", "Property", "Such as extent or primvars:st", Type::Text, true},
          {"authored", "It must be authored", "A schema's fallback isn't enough", Type::Bool, false}}},
        {"value", "Attribute value", "An attribute's value is one of a list, or in a range. Prims without it are fine.",
         {kTypes, {"attribute", "Attribute", "Such as subdivisionScheme", Type::Text, true},
          {"values", "Values", "Those that are fine; none: any", Type::TextList, false},
          {"minimum", "Minimum", "", Type::Number, false},
          {"maximum", "Maximum", "", Type::Number, false}}},
        {"required_schema", "Required schema", "Prims have an API schema, or are of a type.",
         {kTypes, {"schema", "Schema", "Such as MaterialBindingAPI", Type::Text, true}}},
        {"face_count", "Face count", "Meshes have no more faces than a limit.",
         {{"maximum", "Faces at most", "", Type::Number, true}}},
        {"asset_path", "Asset paths", "Files that attributes name, such as textures, follow a convention.",
         {kTypes, {"pattern", "Pattern", "A regular expression for the whole path as it's authored; empty: any", Type::Text, false},
          {"relative", "Relative paths only", "", Type::Bool, false},
          {"resolves", "The file must be found", "", Type::Bool, false}}},
        {"kind_hierarchy", "Kind hierarchy", "Prims of some kinds are under prims of others.",
         {{"kinds", "Kinds", "Such as component, subcomponent", Type::TextList, true},
          {"under", "Directly under", "The kinds their parent may have; / for the root", Type::TextList, true}}},
    };
    return templates;
}

const CheckTemplate* findCheckTemplate(const std::string& id) {
    for (const CheckTemplate& each : checkTemplates()) {
        if (each.id == id) return &each;
    }
    return nullptr;
}

std::string CheckSettings::displayName() const {
    if (!label.empty()) return label;
    const CheckTemplate* known = findCheckTemplate(templateId);
    return known ? known->label : templateId;
}

std::shared_ptr<const Check> Check::make(const CheckSettings& settings, std::string* error) {
    const CheckTemplate* kind = findCheckTemplate(settings.templateId);
    if (!kind) {
        *error = "there's no kind of check called " + settings.templateId;
        return nullptr;
    }
    const JsObject& values = settings.values;
    for (const TemplateField& field : kind->fields) {
        const JsValue* value = valueOf(values, field.key);
        if (value && !value->IsNull()) {
            const bool fits = field.type == Type::Text       ? value->IsString()
                              : field.type == Type::Bool     ? value->IsBool()
                              : field.type == Type::Number   ? value->IsInt() || value->IsReal()
                                                             : value->IsArrayOf<std::string>();
            if (!fits) {
                *error = field.label + " isn't what it should be";
                return nullptr;
            }
        }
        const bool missing = !value || value->IsNull() || (value->IsString() && value->GetString().empty()) ||
                             (value->IsArray() && value->GetJsArray().empty());
        if (field.required && missing) {
            *error = field.label + " is missing";
            return nullptr;
        }
    }

    auto check = std::make_shared<Check>();
    check->m_rule = settings.ruleName();
    check->m_name = settings.templateId;
    check->m_severity = settings.severity;
    for (const std::string& name : list(values, "types")) {
        const TfType type = schemaType(name);
        if (!type) {
            *error = "USD has no schema " + name;
            return nullptr;
        }
        check->m_types.push_back(type);
    }

    std::regex pattern;
    const std::string patternText = text(values, "pattern");
    try {
        pattern = std::regex(patternText);
    } catch (const std::regex_error&) {
        *error = "the pattern " + patternText + " isn't a regular expression";
        return nullptr;
    }

    const std::string& id = settings.templateId;
    if (id == "name_pattern") {
        check->m_check = [pattern, patternText](const UsdPrim& prim, const Report& report) {
            if (!std::regex_match(prim.GetName().GetString(), pattern)) {
                report(prim.GetPath(), "The name " + prim.GetName().GetString() + " doesn't match " + patternText);
            }
        };
    } else if (id == "required_property") {
        check->m_check = [name = TfToken(text(values, "property")), authored = flag(values, "authored")](
                             const UsdPrim& prim, const Report& report) {
            const UsdProperty property = prim.GetProperty(name);
            if (!property) {
                report(prim.GetPath(), "It has no " + name.GetString());
            } else if (authored && !property.IsAuthored()) {
                report(prim.GetPath(), name.GetString() + " isn't authored");
            }
        };
    } else if (id == "value") {
        const std::vector<std::string> allowed = list(values, "values");
        const std::optional<double> minimum = number(values, "minimum");
        const std::optional<double> maximum = number(values, "maximum");
        if (allowed.empty() && !minimum && !maximum) {
            *error = "it needs values, a minimum or a maximum";
            return nullptr;
        }
        check->m_check = [name = TfToken(text(values, "attribute")), allowed, minimum, maximum](const UsdPrim& prim,
                                                                                               const Report& report) {
            const UsdAttribute attribute = prim.GetAttribute(name);
            VtValue value;
            if (!attribute || !attribute.Get(&value, UsdTimeCode::EarliestTime())) return;
            const std::string shown = asText(value);
            if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), shown) == allowed.end()) {
                report(attribute.GetPath(), name.GetString() + " is " + shown + ", not one of " + TfStringJoin(allowed, ", "));
                return;
            }
            if (!minimum && !maximum) return;
            const std::optional<double> amount = asNumber(value);
            if (!amount) {
                report(attribute.GetPath(), name.GetString() + " isn't a number");
            } else if (minimum && *amount < *minimum) {
                report(attribute.GetPath(), name.GetString() + " is " + shown + ", below " + numberText(minimum));
            } else if (maximum && *amount > *maximum) {
                report(attribute.GetPath(), name.GetString() + " is " + shown + ", above " + numberText(maximum));
            }
        };
    } else if (id == "required_schema") {
        const std::string name = text(values, "schema");
        const TfType type = schemaType(name);
        if (!type) {
            *error = "USD has no schema " + name;
            return nullptr;
        }
        check->m_check = [type, name](const UsdPrim& prim, const Report& report) {
            if (!hasSchema(prim, type)) report(prim.GetPath(), "It has no " + name);
        };
    } else if (id == "face_count") {
        check->m_check = [maximum = *number(values, "maximum")](const UsdPrim& prim, const Report& report) {
            const UsdGeomMesh mesh(prim);
            if (!mesh) return;
            const size_t faces = mesh.GetFaceCount(UsdTimeCode::EarliestTime());
            if (double(faces) > maximum) {
                report(prim.GetPath(), "It has " + std::to_string(faces) + " faces, more than " + numberText(maximum));
            }
        };
    } else if (id == "asset_path") {
        const bool relative = flag(values, "relative");
        const bool resolves = flag(values, "resolves");
        if (patternText.empty() && !relative && !resolves) {
            *error = "it needs a pattern, or one of the others";
            return nullptr;
        }
        check->m_check = [pattern, patternText, relative, resolves](const UsdPrim& prim, const Report& report) {
            for (const UsdAttribute& attribute : prim.GetAuthoredAttributes()) {
                if (attribute.GetTypeName().GetScalarType() != SdfValueTypeNames->Asset) continue;
                VtValue value;
                if (!attribute.Get(&value, UsdTimeCode::EarliestTime())) continue;
                std::vector<SdfAssetPath> paths;
                if (value.IsHolding<SdfAssetPath>()) {
                    paths.push_back(value.UncheckedGet<SdfAssetPath>());
                } else if (value.IsHolding<VtArray<SdfAssetPath>>()) {
                    const auto& array = value.UncheckedGet<VtArray<SdfAssetPath>>();
                    paths.assign(array.cbegin(), array.cend());
                }
                for (const SdfAssetPath& path : paths) {
                    const std::string& authored = path.GetAssetPath();
                    if (authored.empty()) continue;
                    if (!patternText.empty() && !std::regex_match(authored, pattern)) {
                        report(attribute.GetPath(), authored + " doesn't match " + patternText);
                    }
                    // A drive or a root, or a server's URL.
                    const bool absolute = authored[0] == '/' || authored[0] == '\\' ||
                                          authored.find("://") != std::string::npos ||
                                          (authored.size() > 1 && authored[1] == ':');
                    if (relative && absolute) report(attribute.GetPath(), authored + " isn't a relative path");
                    // A UDIM's <UDIM> stands for files, and doesn't resolve itself.
                    if (resolves && path.GetResolvedPath().empty() && authored.find("<UDIM>") == std::string::npos) {
                        report(attribute.GetPath(), authored + " can't be found");
                    }
                }
            }
        };
    } else if (id == "kind_hierarchy") {
        check->m_check = [kinds = list(values, "kinds"), under = list(values, "under")](const UsdPrim& prim,
                                                                                       const Report& report) {
            TfToken kind;
            if (!UsdModelAPI(prim).GetKind(&kind) ||
                std::find(kinds.begin(), kinds.end(), kind.GetString()) == kinds.end()) {
                return;
            }
            const UsdPrim parent = prim.GetParent();
            TfToken parentKind;
            if (parent && !parent.IsPseudoRoot()) UsdModelAPI(parent).GetKind(&parentKind);
            const std::string found = !parent || parent.IsPseudoRoot() ? "/" : parentKind.GetString();
            if (std::find(under.begin(), under.end(), found) == under.end()) {
                report(prim.GetPath(), "A " + kind.GetString() + " is under " +
                                           (found == "/" ? "the root" : found.empty() ? "a prim of no kind" : "a " + found));
            }
        };
    }
    return check;
}

std::vector<Finding> Check::run(const UsdPrim& prim) const {
    std::vector<Finding> found;
    if (!m_types.empty() &&
        std::none_of(m_types.begin(), m_types.end(), [&prim](const TfType& type) { return hasSchema(prim, type); })) {
        return found;
    }
    m_check(prim, [&](const SdfPath& site, const std::string& message) {
        found.push_back({m_rule, m_name, m_severity, message, {Site{site, {}}}});
    });
    return found;
}

std::string checkAsPython(const CheckSettings& settings) {
    const JsObject& values = settings.values;
    const std::string& id = settings.templateId;
    const std::string report =
        settings.severity == Severity::Error ? "report.error" : settings.severity == Severity::Warning ? "report.warn" : "report.info";
    std::string function;
    for (const char c : settings.id) function += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (function.empty() || std::isdigit(static_cast<unsigned char>(function[0]))) function = "rule_" + function;

    std::string imports = "import usdcc.validation as v\n";
    std::vector<std::string> types = list(values, "types");
    std::string arguments;
    std::string body;
    const std::string pattern = text(values, "pattern");
    if (id == "name_pattern") {
        imports = "import re\n\n" + imports;
        arguments = ", pattern=" + quoted(pattern);
        body = "    if not re.fullmatch(pattern, prim.GetName()):\n"
               "        " + report + "(prim, \"The name doesn't match \" + pattern)\n";
    } else if (id == "required_property") {
        arguments = ", name=" + quoted(text(values, "property"));
        body = std::string("    found = prim.GetProperty(name)\n    if not found") +
               (flag(values, "authored") ? " or not found.IsAuthored()" : "") + ":\n        " + report +
               "(prim, \"It has no \" + name)\n";
    } else if (id == "value") {
        imports += "from pxr import Usd\n";
        arguments = ", name=" + quoted(text(values, "attribute"));
        body = "    attribute = prim.GetAttribute(name)\n"
               "    value = attribute.Get(Usd.TimeCode.EarliestTime()) if attribute else None\n"
               "    if value is None:\n        return\n";
        if (const auto allowed = list(values, "values"); !allowed.empty()) {
            body += "    shown = str(value).lower() if isinstance(value, bool) else str(value)\n    if shown not in " +
                    quoted(allowed) + ":\n        " + report + "(attribute, \"%s is %s\" % (name, value))\n        return\n";
        }
        if (const auto minimum = number(values, "minimum")) {
            body += "    if value < " + numberText(minimum) + ":\n        " + report + "(attribute, \"%s is %s\" % (name, value))\n";
        }
        if (const auto maximum = number(values, "maximum")) {
            body += "    if value > " + numberText(maximum) + ":\n        " + report + "(attribute, \"%s is %s\" % (name, value))\n";
        }
    } else if (id == "required_schema") {
        arguments = ", schema=" + quoted(text(values, "schema"));
        body = "    applied = [name.split(\":\")[0] for name in prim.GetAppliedSchemas()]\n"
               "    if schema not in applied and not prim.IsA(schema):\n"
               "        " + report + "(prim, \"It has no \" + schema)\n";
    } else if (id == "face_count") {
        imports += "from pxr import Usd, UsdGeom\n";
        types = {"Mesh"};
        arguments = ", maximum=" + numberText(number(values, "maximum"));
        body = "    faces = UsdGeom.Mesh(prim).GetFaceCount(Usd.TimeCode.EarliestTime())\n"
               "    if faces > maximum:\n"
               "        " + report + "(prim, \"It has %d faces, more than %d\" % (faces, maximum))\n";
    } else if (id == "asset_path") {
        imports = "import os\nimport re\n\n" + imports + "from pxr import Sdf, Usd\n";
        arguments = ", pattern=" + quoted(pattern);
        body = "    for attribute in prim.GetAuthoredAttributes():\n"
               "        value = attribute.Get(Usd.TimeCode.EarliestTime())\n"
               "        for path in [value] if isinstance(value, Sdf.AssetPath) else value if isinstance(value, Sdf.AssetPathArray) else []:\n"
               "            if not path.path:\n                continue\n";
        if (!pattern.empty()) {
            body += "            if not re.fullmatch(pattern, path.path):\n                " + report +
                    "(attribute, path.path + \" doesn't match \" + pattern)\n";
        }
        if (flag(values, "relative")) {
            body += "            if os.path.isabs(path.path) or \"://\" in path.path:\n                " + report +
                    "(attribute, path.path + \" isn't a relative path\")\n";
        }
        if (flag(values, "resolves")) {
            body += "            if not path.resolvedPath and \"<UDIM>\" not in path.path:\n                " + report +
                    "(attribute, path.path + \" can't be found\")\n";
        }
    } else if (id == "kind_hierarchy") {
        imports += "from pxr import Usd\n";
        body = "    if Usd.ModelAPI(prim).GetKind() not in " + quoted(list(values, "kinds")) +
               ":\n        return\n"
               "    parent = prim.GetParent()\n"
               "    kind = \"/\" if parent.IsPseudoRoot() else Usd.ModelAPI(parent).GetKind()\n"
               "    if kind not in " + quoted(list(values, "under")) + ":\n        " + report +
               "(prim, \"It's under %s\" % (kind or \"a prim of no kind\"))\n";
    }
    return imports + "\n\n@v.rule(" + quoted("my." + settings.id) + ", " + quoted(settings.displayName()) +
           (types.empty() ? std::string() : ", types=" + quoted(types)) + ")\ndef " + function + "(prim, report" + arguments +
           "):\n" + (body.empty() ? "    pass\n" : body);
}

}  // namespace usdcc::usd::validation
