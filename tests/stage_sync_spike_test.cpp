// Phase 0 of docs/SHARED_STAGE.md: a spike, not the design. A layer's state delegate
// records every primitive change as an op (§4), the ops are replayed onto a second
// layer that started equal, and the two must end identical. The ops' values travel
// through a scratch layer in USDA or USDC, the plan's idea for serializing them; the
// mesh test times that on a million points. The CHECKs are the plan's assumptions, so
// a failure here is a finding, not a flake. Run with "stage_sync" as the filter.
#include "test_harness.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/kind/registry.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerStateDelegate.h>
#include <pxr/usd/sdf/namespaceEdit.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/inherits.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace PXR_NS;

namespace {

// ---------------------------------------------------------------------------
// The ops: one per call the layer makes to its state delegate.

enum class OpKind { SetField, SetDictValue, SetTimeSample, CreateSpec, DeleteSpec, MoveSpec, PushToken, PushPath, PopToken, PopPath };

struct Op {
    OpKind kind = OpKind::SetField;
    SdfPath path;       // the spec, or the parent for a push or pop
    SdfPath otherPath;  // MoveSpec's new path; PushPath's and PopPath's value
    TfToken field;      // the field; for a push or pop, the children field
    TfToken token;      // SetDictValue's key path; PushToken's and PopToken's value
    double time = 0;
    SdfSpecType specType = SdfSpecTypeUnknown;  // CreateSpec's; for a value op, the spec's when recorded
    bool inert = false;
    VtValue value;      // empty: erase
    TfToken typeName;   // for a value op on an attribute: its typeName when recorded
};

bool hasValue(OpKind kind) {
    return kind == OpKind::SetField || kind == OpKind::SetDictValue || kind == OpKind::SetTimeSample;
}

// Records each change as the call itself (EditRecorder in undo_manager.cpp keeps its
// inverse instead), and replays ops through the delegate's own public calls, which
// change the layer directly, as undo does.
class OpRecorder : public SdfLayerStateDelegateBase {
public:
    static TfRefPtr<OpRecorder> install(const SdfLayerHandle& layer) {
        TfRefPtr<OpRecorder> recorder = TfCreateRefPtr(new OpRecorder());
        layer->SetStateDelegate(recorder);
        return recorder;
    }

    bool recording = true;
    std::vector<Op> ops;

    void apply(const Op& op) {
        switch (op.kind) {
        case OpKind::SetField: SetField(op.path, op.field, op.value); break;
        case OpKind::SetDictValue: SetFieldDictValueByKey(op.path, op.field, op.token, op.value); break;
        case OpKind::SetTimeSample: SetTimeSample(op.path, op.time, op.value); break;
        case OpKind::CreateSpec: CreateSpec(op.path, op.specType, op.inert); break;
        case OpKind::DeleteSpec: DeleteSpec(op.path, op.inert); break;
        case OpKind::MoveSpec: MoveSpec(op.path, op.otherPath); break;
        case OpKind::PushToken: PushChild(op.path, op.field, op.token); break;
        case OpKind::PushPath: PushChild(op.path, op.field, op.otherPath); break;
        case OpKind::PopToken: PopChild(op.path, op.field, op.token); break;
        case OpKind::PopPath: PopChild(op.path, op.field, op.otherPath); break;
        }
    }

    void applyAll(const std::vector<Op>& batch) {
        const bool was = recording;
        recording = false;
        {
            SdfChangeBlock block;
            for (const Op& op : batch) apply(op);
        }
        recording = was;
    }

protected:
    bool _IsDirty() override { return m_dirty; }
    void _MarkCurrentStateAsClean() override { m_dirty = false; }
    void _MarkCurrentStateAsDirty() override { m_dirty = true; }
    void _OnSetLayer(const SdfLayerHandle&) override {}

    void _OnSetField(const SdfPath& path, const TfToken& field, const VtValue& value) override {
        Op op;
        op.kind = OpKind::SetField;
        op.path = path;
        op.field = field;
        op.value = value;
        valueOp(std::move(op));
    }
    void _OnSetField(const SdfPath& path, const TfToken& field, const SdfAbstractDataConstValue& value) override {
        VtValue v;
        value.GetValue(&v);
        _OnSetField(path, field, v);
    }
    void _OnSetFieldDictValueByKey(const SdfPath& path, const TfToken& field, const TfToken& keyPath,
                                   const VtValue& value) override {
        Op op;
        op.kind = OpKind::SetDictValue;
        op.path = path;
        op.field = field;
        op.token = keyPath;
        op.value = value;
        valueOp(std::move(op));
    }
    void _OnSetFieldDictValueByKey(const SdfPath& path, const TfToken& field, const TfToken& keyPath,
                                   const SdfAbstractDataConstValue& value) override {
        VtValue v;
        value.GetValue(&v);
        _OnSetFieldDictValueByKey(path, field, keyPath, v);
    }
    void _OnSetTimeSample(const SdfPath& path, double time, const VtValue& value) override {
        Op op;
        op.kind = OpKind::SetTimeSample;
        op.path = path;
        op.time = time;
        op.value = value;
        valueOp(std::move(op));
    }
    void _OnSetTimeSample(const SdfPath& path, double time, const SdfAbstractDataConstValue& value) override {
        VtValue v;
        value.GetValue(&v);
        _OnSetTimeSample(path, time, v);
    }
    void _OnCreateSpec(const SdfPath& path, SdfSpecType specType, bool inert) override {
        Op op;
        op.kind = OpKind::CreateSpec;
        op.path = path;
        op.specType = specType;
        op.inert = inert;
        add(std::move(op));
    }
    void _OnDeleteSpec(const SdfPath& path, bool inert) override {
        Op op;
        op.kind = OpKind::DeleteSpec;
        op.path = path;
        op.inert = inert;
        add(std::move(op));
    }
    void _OnMoveSpec(const SdfPath& oldPath, const SdfPath& newPath) override {
        Op op;
        op.kind = OpKind::MoveSpec;
        op.path = oldPath;
        op.otherPath = newPath;
        add(std::move(op));
    }
    void _OnPushChild(const SdfPath& parent, const TfToken& field, const TfToken& value) override {
        add(childOp(OpKind::PushToken, parent, field, value, {}));
    }
    void _OnPushChild(const SdfPath& parent, const TfToken& field, const SdfPath& value) override {
        add(childOp(OpKind::PushPath, parent, field, {}, value));
    }
    void _OnPopChild(const SdfPath& parent, const TfToken& field, const TfToken& oldValue) override {
        add(childOp(OpKind::PopToken, parent, field, oldValue, {}));
    }
    void _OnPopChild(const SdfPath& parent, const TfToken& field, const SdfPath& oldValue) override {
        add(childOp(OpKind::PopPath, parent, field, {}, oldValue));
    }

private:
    static Op childOp(OpKind kind, const SdfPath& parent, const TfToken& field, const TfToken& token,
                      const SdfPath& path) {
        Op op;
        op.kind = kind;
        op.path = parent;
        op.field = field;
        op.token = token;
        op.otherPath = path;
        return op;
    }
    // Called before the change, so the spec's type (and an attribute's typeName)
    // can still be read: the scratch layer needs them to hold the value.
    void valueOp(Op op) {
        if (!recording) {
            m_dirty = true;
            return;
        }
        const SdfLayerHandle layer = _GetLayer();
        op.specType = layer->GetSpecType(op.path);
        if (op.specType == SdfSpecTypeAttribute) {
            op.typeName = layer->GetFieldAs<TfToken>(op.path, SdfFieldKeys->TypeName);
        }
        add(std::move(op));
    }
    void add(Op op) {
        m_dirty = true;
        if (recording) ops.push_back(std::move(op));
    }

    bool m_dirty = false;
};

// ---------------------------------------------------------------------------
// Comparing two layers: every spec, its type, its fields and their values.

std::string firstDifference(const SdfLayerHandle& a, const SdfLayerHandle& b) {
    std::string diff;
    const auto oneWay = [&diff](const SdfLayerHandle& x, const SdfLayerHandle& y, const char* xName) {
        x->Traverse(SdfPath::AbsoluteRootPath(), [&](const SdfPath& path) {
            if (!diff.empty()) return;
            if (!y->HasSpec(path)) {
                diff = path.GetString() + " is only in the " + xName + " layer";
                return;
            }
            if (x->GetSpecType(path) != y->GetSpecType(path)) {
                diff = path.GetString() + " has different spec types";
                return;
            }
            std::vector<TfToken> xFields = x->ListFields(path), yFields = y->ListFields(path);
            std::sort(xFields.begin(), xFields.end());
            std::sort(yFields.begin(), yFields.end());
            if (xFields != yFields) {
                std::string names;
                for (const TfToken& f : xFields) names += " " + f.GetString();
                names += " |";
                for (const TfToken& f : yFields) names += " " + f.GetString();
                diff = path.GetString() + " has different fields:" + names;
                return;
            }
            for (const TfToken& field : xFields) {
                if (x->GetField(path, field) != y->GetField(path, field)) {
                    diff = path.GetString() + " field " + field.GetString() + " differs: " +
                           TfStringify(x->GetField(path, field)) + " vs " + TfStringify(y->GetField(path, field));
                    return;
                }
            }
        });
    };
    oneWay(a, b, "first");
    if (diff.empty()) oneWay(b, a, "second");
    if (diff.empty()) {
        std::string as, bs;
        a->ExportToString(&as);
        b->ExportToString(&bs);
        if (as != bs) diff = "the fields match but the USDA text differs";
    }
    return diff;
}

// ---------------------------------------------------------------------------
// Serializing a batch: a text header of one line per op, and the values in
// scratch layers that mirror the specs they belong to. A (spec, field) set twice
// in one batch goes into a second scratch layer, so a batch is usually one.
// Tokens, token lists and path lists (typeName, the children fields) go in the
// header: in a scratch layer a children field would describe children it hasn't got.

enum class Format { Usda, Usdc };

struct EncodedBatch {
    std::string header;
    std::vector<std::string> layers;  // USDA text, or USDC bytes
    size_t bytes() const {
        size_t n = header.size();
        for (const std::string& l : layers) n += l.size();
        return n;
    }
};

std::filesystem::path tempFile(const char* tag, size_t i) {
    return std::filesystem::temp_directory_path() /
           ("usdcc_stage_sync_spike_" + std::string(tag) + "_" + std::to_string(i) + ".usdc");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Creates the spec `op` sets a value on in the scratch layer, if it isn't there.
bool mirrorSpec(const SdfLayerHandle& scratch, const Op& op) {
    const SdfPath& path = op.path;
    if (scratch->HasSpec(path)) return true;
    switch (op.specType) {
    case SdfSpecTypePrim:
    case SdfSpecTypeVariant: return SdfJustCreatePrimInLayer(scratch, path);
    case SdfSpecTypeAttribute: {
        const SdfValueTypeName typeName = SdfSchema::GetInstance().FindType(op.typeName);
        return typeName && SdfJustCreatePrimAttributeInLayer(scratch, path, typeName);
    }
    case SdfSpecTypeRelationship: {
        const SdfPrimSpecHandle prim = SdfCreatePrimInLayer(scratch, path.GetParentPath());
        return prim && SdfRelationshipSpec::New(prim, path.GetName());
    }
    default: return false;
    }
}

template <class T>
std::string joined(const std::vector<T>& items) {
    std::string s;
    for (const T& item : items) s += (s.empty() ? "" : ",") + item.GetString();
    return s;
}

// `unsupported` collects the ops whose values couldn't be stored.
EncodedBatch encode(const std::vector<Op>& ops, Format format, std::vector<std::string>* unsupported) {
    EncodedBatch batch;
    std::vector<SdfLayerRefPtr> scratch;
    std::map<std::string, size_t> uses;
    std::ostringstream header;
    char time[64];
    for (size_t i = 0; i < ops.size(); ++i) {
        Op op = ops[i];
        // Sdf sets a new attribute's `custom` before its typeName, so that op was
        // recorded without one. The typeName is set later in the same batch.
        if (op.specType == SdfSpecTypeAttribute && op.typeName.IsEmpty()) {
            for (size_t j = i + 1; j < ops.size(); ++j) {
                const Op& later = ops[j];
                if (later.kind == OpKind::SetField && later.path == op.path &&
                    later.field == SdfFieldKeys->TypeName && later.value.IsHolding<TfToken>()) {
                    op.typeName = later.value.UncheckedGet<TfToken>();
                    break;
                }
            }
        }
        std::string ref = "-";
        if (hasValue(op.kind)) {
            if (op.value.IsEmpty()) {
                ref = "e";
            } else if (op.value.IsHolding<TfToken>()) {
                ref = "k" + op.value.UncheckedGet<TfToken>().GetString();
            } else if (op.value.IsHolding<TfTokenVector>()) {
                ref = "t" + joined(op.value.UncheckedGet<TfTokenVector>());
            } else if (op.value.IsHolding<SdfPathVector>()) {
                ref = "p" + joined(op.value.UncheckedGet<SdfPathVector>());
            } else {
                std::snprintf(time, sizeof time, "%.17g", op.time);
                const std::string key = op.path.GetString() + "|" + op.field.GetString() + "|" + op.token.GetString() +
                                        "|" + (op.kind == OpKind::SetTimeSample ? time : "");
                const size_t slot = uses[key]++;
                while (scratch.size() <= slot) scratch.push_back(SdfLayer::CreateAnonymous("scratch.usda"));
                const SdfLayerHandle layer = scratch[slot];
                bool stored = mirrorSpec(layer, op);
                if (stored) {
                    if (op.kind == OpKind::SetField) layer->SetField(op.path, op.field, op.value);
                    else if (op.kind == OpKind::SetDictValue) layer->SetFieldDictValueByKey(op.path, op.field, op.token, op.value);
                    else layer->SetTimeSample(op.path, op.time, op.value);
                }
                if (stored) {
                    ref = "s" + std::to_string(slot);
                } else {
                    ref = "x";
                    if (unsupported) {
                        unsupported->push_back(op.path.GetString() + " " + op.field.GetString() + " (" +
                                               op.value.GetTypeName() + ")");
                    }
                }
            }
        }
        std::snprintf(time, sizeof time, "%.17g", op.time);
        header << int(op.kind) << '\t' << op.path.GetString() << '\t' << op.otherPath.GetString() << '\t'
               << op.field.GetString() << '\t' << op.token.GetString() << '\t' << time << '\t' << int(op.specType)
               << '\t' << int(op.inert) << '\t' << ref << '\n';
    }
    batch.header = header.str();
    for (size_t i = 0; i < scratch.size(); ++i) {
        std::string data;
        if (format == Format::Usda) {
            scratch[i]->ExportToString(&data);
        } else {
            const std::filesystem::path file = tempFile("encode", i);
            scratch[i]->Export(file.string());
            data = readFile(file);
            std::filesystem::remove(file);
        }
        batch.layers.push_back(std::move(data));
    }
    return batch;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

std::vector<Op> decode(const EncodedBatch& batch, Format format) {
    std::vector<SdfLayerRefPtr> scratch;
    for (size_t i = 0; i < batch.layers.size(); ++i) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("scratch.usda");
        if (format == Format::Usda) {
            layer->ImportFromString(batch.layers[i]);
        } else {
            // USDC can't be read from memory without an ArAsset of our own: through a
            // file, copied into memory so the file can go.
            const std::filesystem::path file = tempFile("decode", i);
            {
                std::ofstream out(file, std::ios::binary);
                out.write(batch.layers[i].data(), std::streamsize(batch.layers[i].size()));
            }
            {
                SdfLayerRefPtr opened = SdfLayer::OpenAsAnonymous(file.string());
                if (opened) layer->TransferContent(opened);
            }
            std::filesystem::remove(file);
        }
        scratch.push_back(layer);
    }

    std::vector<Op> ops;
    for (const std::string& line : split(batch.header, '\n')) {
        if (line.empty()) continue;
        const std::vector<std::string> f = split(line, '\t');
        Op op;
        op.kind = OpKind(std::stoi(f[0]));
        op.path = f[1].empty() ? SdfPath() : SdfPath(f[1]);
        op.otherPath = f[2].empty() ? SdfPath() : SdfPath(f[2]);
        op.field = TfToken(f[3]);
        op.token = TfToken(f[4]);
        op.time = std::stod(f[5]);
        op.specType = SdfSpecType(std::stoi(f[6]));
        op.inert = f[7] == "1";
        const std::string& ref = f[8];
        const std::string rest = ref.substr(1);
        switch (ref[0]) {
        case 'k': op.value = VtValue(TfToken(rest)); break;
        case 't': {
            TfTokenVector tokens;
            if (!rest.empty()) for (const std::string& t : split(rest, ',')) tokens.emplace_back(t);
            op.value = VtValue(tokens);
            break;
        }
        case 'p': {
            SdfPathVector paths;
            if (!rest.empty()) for (const std::string& p : split(rest, ',')) paths.emplace_back(p);
            op.value = VtValue(paths);
            break;
        }
        case 's': {
            const SdfLayerHandle layer = scratch[std::stoul(rest)];
            if (op.kind == OpKind::SetField) op.value = layer->GetField(op.path, op.field);
            else if (op.kind == OpKind::SetDictValue) op.value = layer->GetFieldDictValueByKey(op.path, op.field, op.token);
            else layer->QueryTimeSample(op.path, op.time, &op.value);
            break;
        }
        default: break;  // '-', 'e', 'x': no value
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

// ---------------------------------------------------------------------------
// The edits: through UsdStage, as the app makes them, plus Sdf's own namespace
// edits for rename and reparent.

void editNewScene(const UsdStageRefPtr& stage) {
    const SdfLayerHandle layer = stage->GetRootLayer();
    UsdGeomXform world = UsdGeomXform::Define(stage, SdfPath("/World"));
    stage->SetDefaultPrim(world.GetPrim());
    stage->SetStartTimeCode(1);
    stage->SetEndTimeCode(24);
    stage->SetMetadataByDictKey(SdfFieldKeys->CustomLayerData, TfToken("usdcc:who"), VtValue(std::string("spike")));
    UsdModelAPI(world.GetPrim()).SetKind(KindTokens->component);

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Cube"));
    mesh.CreatePointsAttr(VtValue(VtVec3fArray{{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
                                               {-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1}}));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray{4, 4, 4, 4, 4, 4}));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray{0, 1, 3, 2, 4, 6, 7, 5, 0, 4, 5, 1,
                                                        2, 3, 7, 6, 0, 2, 6, 4, 1, 5, 7, 3}));
    UsdGeomPrimvarsAPI(mesh.GetPrim())
        .CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->constant)
        .Set(VtVec3fArray{GfVec3f(1, 0, 0)});
    UsdGeomXformCommonAPI(mesh.GetPrim()).SetTranslate(GfVec3d(1, 2, 3));
    UsdGeomXformOp rotate = mesh.AddRotateYOp();
    rotate.Set(0.f, UsdTimeCode(1));
    rotate.Set(90.f, UsdTimeCode(12));
    rotate.Set(180.f, UsdTimeCode(24));
    rotate.Set(45.f, UsdTimeCode(12));  // a sample replaced
    mesh.GetPrim().SetCustomDataByKey(TfToken("usdcc:note"), VtValue(std::string("hi")));
    mesh.GetPrim().SetCustomDataByKey(TfToken("a:b:c"), VtValue(3.5));
    mesh.GetPrim().SetDocumentation("A cube");

    // Values of many types, and some cleared again.
    const UsdPrim w = world.GetPrim();
    w.CreateAttribute(TfToken("matrix"), SdfValueTypeNames->Matrix4d).Set(GfMatrix4d(1).SetTranslate(GfVec3d(4, 5, 6)));
    w.CreateAttribute(TfToken("quat"), SdfValueTypeNames->Quath).Set(GfQuath(0.5, GfVec3h(0.5, 0.5, 0.5)));
    w.CreateAttribute(TfToken("names"), SdfValueTypeNames->StringArray).Set(VtStringArray{"a", "b c", "d\"e"});
    w.CreateAttribute(TfToken("texture"), SdfValueTypeNames->Asset).Set(SdfAssetPath("./tex.png"));
    w.CreateAttribute(TfToken("mode"), SdfValueTypeNames->Token, false, SdfVariabilityUniform).Set(TfToken("fast"));
    w.CreateAttribute(TfToken("tiny"), SdfValueTypeNames->Double).Set(0.1 + 0.2);
    w.CreateAttribute(TfToken("ratio"), SdfValueTypeNames->Float).Set(1.0f / 3.0f);
    UsdAttribute temp = w.CreateAttribute(TfToken("temp"), SdfValueTypeNames->Float);
    temp.Set(1.f);
    temp.ClearDefault();
    temp.Set(2.f, UsdTimeCode(5));
    temp.Set(3.f, UsdTimeCode(6));
    temp.ClearAtTime(UsdTimeCode(5));
    w.CreateRelationship(TfToken("lookAt")).AddTarget(SdfPath("/World/Cube"));

    // Composition arcs: list ops.
    UsdPrim ref = stage->DefinePrim(SdfPath("/World/Ref"));
    stage->CreateClassPrim(SdfPath("/_class_Thing"));
    ref.GetReferences().AddInternalReference(SdfPath("/World/Cube"));
    ref.GetPayloads().AddInternalPayload(SdfPath("/World/Cube"));
    ref.GetInherits().AddInherit(SdfPath("/_class_Thing"));

    // A material, its shader connected, bound to the cube: apiSchemas and targets.
    UsdShadeMaterial material = UsdShadeMaterial::Define(stage, SdfPath("/World/Looks/Red"));
    UsdShadeShader shader = UsdShadeShader::Define(stage, SdfPath("/World/Looks/Red/Surface"));
    shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(1, 0, 0));
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), TfToken("surface"));
    UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim()).Bind(material);

    // A variant set, and edits inside a variant.
    UsdVariantSet look = w.GetVariantSets().AddVariantSet("look");
    look.AddVariant("red");
    look.AddVariant("blue");
    look.SetVariantSelection("red");
    {
        UsdEditContext context(look.GetVariantEditContext());
        UsdGeomSphere::Define(stage, SdfPath("/World/Extra")).CreateRadiusAttr(VtValue(2.0));
    }
    look.SetVariantSelection("blue");

    // Namespace edits: rename, reparent, reorder, delete, deactivate.
    layer->GetPrimAtPath(SdfPath("/World/Ref"))->SetName("Reference");
    SdfBatchNamespaceEdit reparent;
    reparent.Add(SdfNamespaceEdit::Reparent(SdfPath("/World/Reference"), SdfPath("/World/Looks"), 0));
    CHECK(layer->Apply(reparent), "the reparent applies");
    w.SetChildrenReorder({TfToken("Looks"), TfToken("Cube")});
    UsdPrim doomed = stage->DefinePrim(SdfPath("/World/Doomed"), TfToken("Scope"));
    doomed.CreateAttribute(TfToken("x"), SdfValueTypeNames->Int).Set(1);
    stage->RemovePrim(SdfPath("/World/Doomed"));
    mesh.GetPrim().RemoveProperty(TfToken("primvars:displayColor"));
    stage->DefinePrim(SdfPath("/World/Off")).SetActive(false);
}

// Edits to a scene the other side already has: values changed, specs deleted,
// renamed, a dictionary key erased.
void editExistingScene(const UsdStageRefPtr& stage) {
    const SdfLayerHandle layer = stage->GetRootLayer();
    UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath("/World/Cube")));
    VtVec3fArray points;
    mesh.GetPointsAttr().Get(&points);
    for (GfVec3f& p : points) p *= 2.f;
    mesh.GetPointsAttr().Set(points);
    mesh.GetPrim().ClearCustomDataByKey(TfToken("a:b:c"));
    stage->GetPrimAtPath(SdfPath("/World")).GetVariantSet("look").SetVariantSelection("red");
    stage->RemovePrim(SdfPath("/World/Looks/Reference"));
    layer->GetPrimAtPath(SdfPath("/World/Cube"))->SetName("Box");
    stage->GetPrimAtPath(SdfPath("/World")).GetAttribute(TfToken("quat")).Clear();
    stage->GetPrimAtPath(SdfPath("/World/Off")).SetActive(true);
}

// Replays `ops` onto `target` directly and through each format, checking each
// ends equal to `expected`. `target` is left as it started.
void checkReplays(const char* name, const std::vector<Op>& ops, const SdfLayerHandle& expected,
                  const SdfLayerHandle& base) {
    const auto replayOnto = [&](const std::vector<Op>& batch) {
        SdfLayerRefPtr target = SdfLayer::CreateAnonymous("target.usda");
        target->TransferContent(base);
        TfRefPtr<OpRecorder> recorder = OpRecorder::install(target);
        recorder->applyAll(batch);
        return firstDifference(expected, target);
    };

    const std::string direct = replayOnto(ops);
    CHECK(direct.empty(), "%s: replaying the ops in memory gives an identical layer: %s", name, direct.c_str());

    for (const Format format : {Format::Usda, Format::Usdc}) {
        const char* formatName = format == Format::Usda ? "usda" : "usdc";
        std::vector<std::string> unsupported;
        const EncodedBatch batch = encode(ops, format, &unsupported);
        std::string list;
        for (const std::string& u : unsupported) list += "\n    " + u;
        CHECK(unsupported.empty(), "%s/%s: every value fits a scratch layer; not:%s", name, formatName, list.c_str());
        const std::string viaFormat = replayOnto(decode(batch, format));
        CHECK(viaFormat.empty(), "%s/%s: replaying the decoded ops gives an identical layer: %s", name, formatName,
              viaFormat.c_str());
        std::printf("  %s/%s: %zu ops, %zu scratch layer(s), %zu bytes\n", name, formatName, ops.size(),
                    batch.layers.size(), batch.bytes());
    }
}

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

USDCC_TEST(stage_sync_spike_new_scene) {
    SdfLayerRefPtr source = SdfLayer::CreateAnonymous("source.usda");
    SdfLayerRefPtr empty = SdfLayer::CreateAnonymous("base.usda");
    TfRefPtr<OpRecorder> recorder = OpRecorder::install(source);
    editNewScene(UsdStage::Open(source));
    CHECK(!recorder->ops.empty(), "the recorder saw the edits");
    checkReplays("new scene", recorder->ops, source, empty);
}

USDCC_TEST(stage_sync_spike_existing_scene) {
    SdfLayerRefPtr source = SdfLayer::CreateAnonymous("source.usda");
    TfRefPtr<OpRecorder> recorder = OpRecorder::install(source);
    recorder->recording = false;
    UsdStageRefPtr stage = UsdStage::Open(source);
    editNewScene(stage);
    SdfLayerRefPtr base = SdfLayer::CreateAnonymous("base.usda");
    base->TransferContent(source);
    recorder->recording = true;
    editExistingScene(stage);
    checkReplays("existing scene", recorder->ops, source, base);
}

// Whole-layer calls a script might make: what do they record?
USDCC_TEST(stage_sync_spike_whole_layer_calls) {
    SdfLayerRefPtr source = SdfLayer::CreateAnonymous("source.usda");
    TfRefPtr<OpRecorder> recorder = OpRecorder::install(source);
    recorder->recording = false;
    editNewScene(UsdStage::Open(source));
    SdfLayerRefPtr base = SdfLayer::CreateAnonymous("base.usda");
    base->TransferContent(source);

    SdfLayerRefPtr other = SdfLayer::CreateAnonymous("other.usda");
    other->ImportFromString("#usda 1.0\ndef Xform \"Other\"\n{\n    double radius = 3\n}\n");
    recorder->recording = true;
    source->TransferContent(other);
    std::printf("  TransferContent recorded %zu ops\n", recorder->ops.size());
    checkReplays("transfer content", recorder->ops, source, base);

    recorder->ops.clear();
    SdfLayerRefPtr before = SdfLayer::CreateAnonymous("before.usda");
    before->TransferContent(source);
    source->Clear();
    std::printf("  Clear recorded %zu ops\n", recorder->ops.size());
    checkReplays("clear", recorder->ops, source, before);
}

// The plan's question: how to serialize a large value. A mesh of a million points
// set once, sent through each format; raw bytes as the floor.
USDCC_TEST(stage_sync_spike_mesh_timing) {
    constexpr size_t kPoints = 1'000'000;
    SdfLayerRefPtr source = SdfLayer::CreateAnonymous("source.usda");
    TfRefPtr<OpRecorder> recorder = OpRecorder::install(source);
    recorder->recording = false;
    UsdStageRefPtr stage = UsdStage::Open(source);
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Mesh"));
    UsdAttribute points = mesh.CreatePointsAttr();
    SdfLayerRefPtr base = SdfLayer::CreateAnonymous("base.usda");
    base->TransferContent(source);

    VtVec3fArray values(kPoints);
    for (size_t i = 0; i < kPoints; ++i) {
        const float t = float(i);
        values[i] = GfVec3f(std::sin(t) * 1.2345678f, std::cos(t * 0.5f) * 98.7654321f, t * 1e-3f);
    }

    recorder->recording = true;
    auto start = std::chrono::steady_clock::now();
    points.Set(values);
    const double recordMs = msSince(start);
    CHECK(recorder->ops.size() == 1, "setting the points records one op (%zu)", recorder->ops.size());

    start = std::chrono::steady_clock::now();
    std::string raw(kPoints * sizeof(GfVec3f), '\0');
    std::memcpy(raw.data(), values.cdata(), raw.size());
    VtVec3fArray back(kPoints);
    std::memcpy(back.data(), raw.data(), raw.size());
    const double rawMs = msSince(start);
    std::printf("  %zu points: set and recorded in %.1f ms; raw copy out and back %.1f ms, %zu bytes\n", kPoints,
                recordMs, rawMs, raw.size());

    for (const Format format : {Format::Usda, Format::Usdc}) {
        const char* formatName = format == Format::Usda ? "usda" : "usdc";
        start = std::chrono::steady_clock::now();
        const EncodedBatch batch = encode(recorder->ops, format, nullptr);
        const double encodeMs = msSince(start);
        start = std::chrono::steady_clock::now();
        const std::vector<Op> ops = decode(batch, format);
        const double decodeMs = msSince(start);

        SdfLayerRefPtr target = SdfLayer::CreateAnonymous("target.usda");
        target->TransferContent(base);
        TfRefPtr<OpRecorder> targetRecorder = OpRecorder::install(target);
        start = std::chrono::steady_clock::now();
        targetRecorder->applyAll(ops);
        const double applyMs = msSince(start);
        const std::string diff = firstDifference(source, target);
        CHECK(diff.empty(), "%s: the million points arrive identical: %s", formatName, diff.c_str());
        std::printf("  %s: encode %.1f ms, decode %.1f ms, apply %.1f ms, %.2f MB\n", formatName, encodeMs,
                    decodeMs, applyMs, double(batch.bytes()) / (1024.0 * 1024.0));
    }
}
