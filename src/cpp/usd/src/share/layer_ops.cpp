#include "usdcc/usd/share/layer_ops.h"

#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layerStateDelegate.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/schema.h>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QTemporaryDir>

#include <atomic>

#include <algorithm>
#include <cmath>
#include <map>

using namespace PXR_NS;

namespace usdcc::usd::share {

namespace {

// A delegate that only applies: for layers nobody records (the confirmed layer, a
// scratch layer). Its public calls change the layer directly.
class Applier : public SdfLayerStateDelegateBase {
public:
    static TfRefPtr<Applier> forLayer(const SdfLayerHandle& layer) {
        TfRefPtr<Applier> applier = TfCreateRefPtrFromProtectedWeakPtr(
            TfDynamic_cast<TfWeakPtr<Applier>>(layer->GetStateDelegate()));
        if (!applier) {
            applier = TfCreateRefPtr(new Applier());
            layer->SetStateDelegate(applier);
        }
        return applier;
    }

protected:
    bool _IsDirty() override { return m_dirty; }
    void _MarkCurrentStateAsClean() override { m_dirty = false; }
    void _MarkCurrentStateAsDirty() override { m_dirty = true; }
    void _OnSetLayer(const SdfLayerHandle&) override {}
    void _OnSetField(const SdfPath&, const TfToken&, const VtValue&) override { m_dirty = true; }
    void _OnSetField(const SdfPath&, const TfToken&, const SdfAbstractDataConstValue&) override { m_dirty = true; }
    void _OnSetFieldDictValueByKey(const SdfPath&, const TfToken&, const TfToken&, const VtValue&) override {
        m_dirty = true;
    }
    void _OnSetFieldDictValueByKey(const SdfPath&, const TfToken&, const TfToken&,
                                   const SdfAbstractDataConstValue&) override {
        m_dirty = true;
    }
    void _OnSetTimeSample(const SdfPath&, double, const VtValue&) override { m_dirty = true; }
    void _OnSetTimeSample(const SdfPath&, double, const SdfAbstractDataConstValue&) override { m_dirty = true; }
    void _OnCreateSpec(const SdfPath&, SdfSpecType, bool) override { m_dirty = true; }
    void _OnDeleteSpec(const SdfPath&, bool) override { m_dirty = true; }
    void _OnMoveSpec(const SdfPath&, const SdfPath&) override { m_dirty = true; }
    void _OnPushChild(const SdfPath&, const TfToken&, const TfToken&) override { m_dirty = true; }
    void _OnPushChild(const SdfPath&, const TfToken&, const SdfPath&) override { m_dirty = true; }
    void _OnPopChild(const SdfPath&, const TfToken&, const TfToken&) override { m_dirty = true; }
    void _OnPopChild(const SdfPath&, const TfToken&, const SdfPath&) override { m_dirty = true; }

private:
    bool m_dirty = false;
};

// The parent a spec needs before it can be created, if it is one whose parent is
// plain to see: a prim's or a property's. Empty for the others (variant sets and
// variants), which are trusted.
SdfPath requiredParent(const SdfPath& path) {
    if (path.IsPrimPath() || path.IsPrimPropertyPath()) {
        const SdfPath parent = path.GetParentPath();
        if (parent.IsPrimPath() || parent.IsAbsoluteRootPath() || parent.IsPrimVariantSelectionPath()) return parent;
    }
    return {};
}

template <class T>
bool childListHas(const SdfLayerHandle& layer, const SdfPath& parent, const TfToken& field, const T& value) {
    const std::vector<T> list = layer->GetFieldAs<std::vector<T>>(parent, field);
    return std::find(list.begin(), list.end(), value) != list.end();
}

// Sdf pops a child list's last entry. A peer may have pushed others after it since,
// so a child that isn't last is taken out where it is instead.
template <class T>
void popChild(SdfLayerStateDelegateBase& d, const SdfLayerHandle& layer, const SdfPath& parent, const TfToken& field,
              const T& value) {
    std::vector<T> list = layer->GetFieldAs<std::vector<T>>(parent, field);
    if (!list.empty() && list.back() == value) {
        d.PopChild(parent, field, value);
        return;
    }
    list.erase(std::remove(list.begin(), list.end(), value), list.end());
    d.SetField(parent, field, list.empty() ? VtValue() : VtValue(list));
}

bool fits(const SdfLayerHandle& layer, const LayerOp& op) {
    using Kind = LayerOp::Kind;
    switch (op.kind) {
    case Kind::SetField:
    case Kind::SetDictValue:
    case Kind::SetTimeSample: return layer->HasSpec(op.path);
    case Kind::CreateSpec: {
        if (layer->HasSpec(op.path)) return false;
        const SdfPath parent = requiredParent(op.path);
        return parent.IsEmpty() || layer->HasSpec(parent);
    }
    case Kind::DeleteSpec: return layer->HasSpec(op.path);
    case Kind::MoveSpec: return layer->HasSpec(op.path) && !layer->HasSpec(op.otherPath);
    case Kind::PushToken:
        return layer->HasSpec(op.path) && !childListHas(layer, op.path, op.field, op.token);
    case Kind::PushPath:
        return layer->HasSpec(op.path) && !childListHas(layer, op.path, op.field, op.otherPath);
    case Kind::PopToken: return layer->HasSpec(op.path) && childListHas(layer, op.path, op.field, op.token);
    case Kind::PopPath: return layer->HasSpec(op.path) && childListHas(layer, op.path, op.field, op.otherPath);
    }
    return false;
}

void applyOne(SdfLayerStateDelegateBase& d, const SdfLayerHandle& layer, const LayerOp& op) {
    using Kind = LayerOp::Kind;
    switch (op.kind) {
    case Kind::SetField: d.SetField(op.path, op.field, op.value); break;
    case Kind::SetDictValue: d.SetFieldDictValueByKey(op.path, op.field, op.token, op.value); break;
    case Kind::SetTimeSample: d.SetTimeSample(op.path, op.time, op.value); break;
    case Kind::CreateSpec: d.CreateSpec(op.path, op.specType, op.inert); break;
    case Kind::DeleteSpec: d.DeleteSpec(op.path, op.inert); break;
    case Kind::MoveSpec: d.MoveSpec(op.path, op.otherPath); break;
    case Kind::PushToken: d.PushChild(op.path, op.field, op.token); break;
    case Kind::PushPath: d.PushChild(op.path, op.field, op.otherPath); break;
    case Kind::PopToken: popChild(d, layer, op.path, op.field, op.token); break;
    case Kind::PopPath: popChild(d, layer, op.path, op.field, op.otherPath); break;
    }
}

LayerOp valueOp(LayerOp::Kind kind, const SdfLayerHandle& layer, const SdfPath& path) {
    LayerOp op;
    op.kind = kind;
    op.path = path;
    if (layer) {
        op.specType = layer->GetSpecType(path);
        if (op.specType == SdfSpecTypeAttribute) {
            op.typeName = layer->GetFieldAs<TfToken>(path, SdfFieldKeys->TypeName);
        }
    }
    return op;
}

LayerOp childOp(LayerOp::Kind kind, const SdfPath& parent, const TfToken& field) {
    LayerOp op;
    op.kind = kind;
    op.path = parent;
    op.field = field;
    return op;
}

// A fresh .usdc path in a folder of this process's own, removed when it ends. USD
// writes beside the file and renames over it, which a QTemporaryFile's would refuse.
QString temporaryUsdcPath() {
    static QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/usdcc_share_XXXXXX"));
    static std::atomic<int> serial{0};
    return dir.filePath(QStringLiteral("%1.usdc").arg(++serial));
}

// Creates the spec `op` sets a value on in the scratch layer, if it isn't there.
bool mirrorSpec(const SdfLayerHandle& scratch, const LayerOp& op, const TfToken& typeName) {
    const SdfPath& path = op.path;
    if (scratch->HasSpec(path)) return true;
    switch (op.specType) {
    case SdfSpecTypePrim:
    case SdfSpecTypeVariant: return SdfJustCreatePrimInLayer(scratch, path);
    case SdfSpecTypeAttribute: {
        // USDC keeps a value with its own type, so the scratch attribute's type only
        // has to be one: the attribute's, else the value's, else any.
        const SdfSchemaBase& schema = SdfSchema::GetInstance();
        SdfValueTypeName type = schema.FindType(typeName);
        if (!type) type = schema.FindType(op.value);
        if (!type) type = SdfValueTypeNames->Token;
        return SdfJustCreatePrimAttributeInLayer(scratch, path, type);
    }
    case SdfSpecTypeRelationship: {
        const SdfPrimSpecHandle prim = SdfCreatePrimInLayer(scratch, path.GetParentPath());
        return prim && SdfRelationshipSpec::New(prim, path.GetName());
    }
    default: return false;
    }
}

template <class T>
QString joined(const std::vector<T>& items) {
    QStringList parts;
    for (const T& item : items) parts << QString::fromStdString(item.GetString());
    return parts.join(QLatin1Char('\n'));
}

QString numberText(double value) { return QString::number(value, 'g', 17); }

}  // namespace

LayerOp setFieldOp(const SdfLayerHandle& layer, const SdfPath& path, const TfToken& field, const VtValue& value) {
    LayerOp op = valueOp(LayerOp::Kind::SetField, layer, path);
    op.field = field;
    op.value = value;
    return op;
}

bool isChildrenField(const TfToken& field) {
    const auto& keys = SdfChildrenKeys;
    return field == keys->PrimChildren || field == keys->PropertyChildren || field == keys->VariantChildren ||
           field == keys->VariantSetChildren || field == keys->ConnectionChildren ||
           field == keys->RelationshipTargetChildren || field == keys->MapperChildren ||
           field == keys->MapperArgChildren || field == keys->ExpressionChildren;
}

std::optional<LayerOp> invertChildOp(const LayerOp& op) {
    using Kind = LayerOp::Kind;
    LayerOp inverse = op;
    switch (op.kind) {
    case Kind::PushToken: inverse.kind = Kind::PopToken; return inverse;
    case Kind::PushPath: inverse.kind = Kind::PopPath; return inverse;
    case Kind::PopToken: inverse.kind = Kind::PushToken; return inverse;
    case Kind::PopPath: inverse.kind = Kind::PushPath; return inverse;
    default: return std::nullopt;
    }
}

namespace {

// The pushes or pops that turn `before` into `after`, if that's all it is.
template <class T>
std::optional<LayerOps> childOps(const SdfPath& parent, const TfToken& field, const std::vector<T>& before,
                                 const std::vector<T>& after) {
    LayerOps ops;
    if (after.size() >= before.size() && std::equal(before.begin(), before.end(), after.begin())) {
        for (size_t i = before.size(); i < after.size(); ++i) ops.push_back(pushChildOp(parent, field, after[i]));
        return ops;
    }
    // Taken out, the rest in the same order.
    size_t j = 0;
    std::vector<T> removed;
    for (const T& item : before) {
        if (j < after.size() && after[j] == item) {
            ++j;
        } else {
            removed.push_back(item);
        }
    }
    if (j != after.size()) return std::nullopt;  // reordered, or new ones among them
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) ops.push_back(popChildOp(parent, field, *it));
    return ops;
}

}  // namespace

LayerOps setFieldOps(const SdfLayerHandle& layer, const SdfPath& path, const TfToken& field, const VtValue& value) {
    if (layer && isChildrenField(field)) {
        const VtValue before = layer->GetField(path, field);
        std::optional<LayerOps> ops;
        if ((value.IsEmpty() || value.IsHolding<TfTokenVector>()) &&
            (before.IsEmpty() || before.IsHolding<TfTokenVector>())) {
            ops = childOps(path, field, before.GetWithDefault<TfTokenVector>(), value.GetWithDefault<TfTokenVector>());
        } else if ((value.IsEmpty() || value.IsHolding<SdfPathVector>()) &&
                   (before.IsEmpty() || before.IsHolding<SdfPathVector>())) {
            ops = childOps(path, field, before.GetWithDefault<SdfPathVector>(), value.GetWithDefault<SdfPathVector>());
        }
        if (ops) return *ops;
    }
    return {setFieldOp(layer, path, field, value)};
}

LayerOp setDictValueOp(const SdfLayerHandle& layer, const SdfPath& path, const TfToken& field,
                       const TfToken& keyPath, const VtValue& value) {
    LayerOp op = valueOp(LayerOp::Kind::SetDictValue, layer, path);
    op.field = field;
    op.token = keyPath;
    op.value = value;
    return op;
}

LayerOp setTimeSampleOp(const SdfLayerHandle& layer, const SdfPath& path, double time, const VtValue& value) {
    LayerOp op = valueOp(LayerOp::Kind::SetTimeSample, layer, path);
    op.time = time;
    op.value = value;
    return op;
}

LayerOp createSpecOp(const SdfPath& path, SdfSpecType type, bool inert) {
    LayerOp op;
    op.kind = LayerOp::Kind::CreateSpec;
    op.path = path;
    op.specType = type;
    op.inert = inert;
    return op;
}

LayerOp deleteSpecOp(const SdfPath& path, bool inert) {
    LayerOp op;
    op.kind = LayerOp::Kind::DeleteSpec;
    op.path = path;
    op.inert = inert;
    return op;
}

LayerOp moveSpecOp(const SdfPath& oldPath, const SdfPath& newPath) {
    LayerOp op;
    op.kind = LayerOp::Kind::MoveSpec;
    op.path = oldPath;
    op.otherPath = newPath;
    return op;
}

LayerOp pushChildOp(const SdfPath& parent, const TfToken& field, const TfToken& value) {
    LayerOp op = childOp(LayerOp::Kind::PushToken, parent, field);
    op.token = value;
    return op;
}

LayerOp pushChildOp(const SdfPath& parent, const TfToken& field, const SdfPath& value) {
    LayerOp op = childOp(LayerOp::Kind::PushPath, parent, field);
    op.otherPath = value;
    return op;
}

LayerOp popChildOp(const SdfPath& parent, const TfToken& field, const TfToken& value) {
    LayerOp op = childOp(LayerOp::Kind::PopToken, parent, field);
    op.token = value;
    return op;
}

LayerOp popChildOp(const SdfPath& parent, const TfToken& field, const SdfPath& value) {
    LayerOp op = childOp(LayerOp::Kind::PopPath, parent, field);
    op.otherPath = value;
    return op;
}

int applyOps(SdfLayerStateDelegateBase& delegate, const SdfLayerHandle& layer, const LayerOps& ops) {
    if (!layer) return int(ops.size());
    int dropped = 0;
    SdfChangeBlock block;
    for (const LayerOp& op : ops) {
        if (fits(layer, op)) {
            applyOne(delegate, layer, op);
        } else {
            ++dropped;
        }
    }
    return dropped;
}

int applyOps(const SdfLayerHandle& layer, const LayerOps& ops) {
    if (!layer) return int(ops.size());
    TfRefPtr<Applier> applier = Applier::forLayer(layer);
    return applyOps(*applier, layer, ops);
}

void appendCoalesced(LayerOps& ops, LayerOp op) {
    if (!ops.empty()) {
        LayerOp& last = ops.back();
        const bool sameField = op.kind == LayerOp::Kind::SetField && last.kind == LayerOp::Kind::SetField &&
                               last.path == op.path && last.field == op.field;
        const bool sameSample = op.kind == LayerOp::Kind::SetTimeSample &&
                                last.kind == LayerOp::Kind::SetTimeSample && last.path == op.path &&
                                last.time == op.time;
        if (sameField || sameSample) {
            if (op.typeName.IsEmpty()) op.typeName = last.typeName;
            last = std::move(op);
            return;
        }
    }
    ops.push_back(std::move(op));
}

qint64 EncodedOps::bytes() const {
    qint64 n = 0;
    for (const QByteArray& layer : layers) n += layer.size();
    return n;
}

std::optional<EncodedOps> encodeOps(const LayerOps& ops, QString* error) {
    EncodedOps encoded;
    std::vector<SdfLayerRefPtr> scratch;
    std::map<std::string, int> uses;
    for (size_t i = 0; i < ops.size(); ++i) {
        const LayerOp& op = ops[i];
        QString ref = QStringLiteral("-");
        if (op.hasValue()) {
            const VtValue& value = op.value;
            if (value.IsEmpty()) {
                ref = QStringLiteral("e");
            } else if (value.IsHolding<TfToken>()) {
                ref = QStringLiteral("k") + QString::fromStdString(value.UncheckedGet<TfToken>().GetString());
            } else if (value.IsHolding<TfTokenVector>()) {
                ref = QStringLiteral("t") + joined(value.UncheckedGet<TfTokenVector>());
            } else if (value.IsHolding<SdfPathVector>()) {
                ref = QStringLiteral("p") + joined(value.UncheckedGet<SdfPathVector>());
            } else {
                // Sdf sets a new attribute's `custom` before its typeName, so that op
                // was recorded without one; the typeName is set later in the batch.
                TfToken typeName = op.typeName;
                if (op.specType == SdfSpecTypeAttribute && typeName.IsEmpty()) {
                    for (size_t j = i + 1; j < ops.size(); ++j) {
                        const LayerOp& later = ops[j];
                        if (later.kind == LayerOp::Kind::SetField && later.path == op.path &&
                            later.field == SdfFieldKeys->TypeName && later.value.IsHolding<TfToken>()) {
                            typeName = later.value.UncheckedGet<TfToken>();
                            break;
                        }
                    }
                }
                const std::string key = op.path.GetString() + "|" + op.field.GetString() + "|" +
                                        op.token.GetString() + "|" +
                                        (op.kind == LayerOp::Kind::SetTimeSample ? numberText(op.time).toStdString()
                                                                                 : std::string());
                const int slot = uses[key]++;
                while (int(scratch.size()) <= slot) scratch.push_back(SdfLayer::CreateAnonymous("scratch.usdc"));
                const SdfLayerHandle layer = scratch[size_t(slot)];
                if (!mirrorSpec(layer, op, typeName)) {
                    if (error) {
                        *error = QStringLiteral("Can't send %1 %2 (%3)")
                                     .arg(QString::fromStdString(op.path.GetString()),
                                          QString::fromStdString(op.field.GetString()),
                                          QString::fromStdString(value.GetTypeName()));
                    }
                    return std::nullopt;
                }
                if (op.kind == LayerOp::Kind::SetField) {
                    layer->SetField(op.path, op.field, value);
                } else if (op.kind == LayerOp::Kind::SetDictValue) {
                    layer->SetFieldDictValueByKey(op.path, op.field, op.token, value);
                } else {
                    layer->SetTimeSample(op.path, op.time, value);
                }
                ref = QStringLiteral("s") + QString::number(slot);
            }
        }
        encoded.header.append(QJsonArray{int(op.kind), QString::fromStdString(op.path.GetString()),
                                         QString::fromStdString(op.otherPath.GetString()),
                                         QString::fromStdString(op.field.GetString()),
                                         QString::fromStdString(op.token.GetString()), numberText(op.time),
                                         int(op.specType), op.inert, ref});
    }
    for (const SdfLayerRefPtr& layer : scratch) {
        std::optional<QByteArray> bytes = layerToUsdc(layer, error);
        if (!bytes) return std::nullopt;
        encoded.layers.push_back(std::move(*bytes));
    }
    return encoded;
}

std::optional<LayerOps> decodeOps(const EncodedOps& encoded, QString* error) {
    std::vector<SdfLayerRefPtr> scratch;
    for (const QByteArray& bytes : encoded.layers) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("scratch.usdc");
        if (!usdcToLayer(bytes, layer, error)) return std::nullopt;
        scratch.push_back(layer);
    }
    const auto path = [](const QJsonValue& v) {
        const QString s = v.toString();
        return s.isEmpty() ? SdfPath() : SdfPath(s.toStdString());
    };
    LayerOps ops;
    ops.reserve(size_t(encoded.header.size()));
    for (const QJsonValue& entryValue : encoded.header) {
        const QJsonArray entry = entryValue.toArray();
        if (entry.size() != 9) {
            if (error) *error = QStringLiteral("An op of the wrong shape");
            return std::nullopt;
        }
        LayerOp op;
        const int kind = entry[0].toInt(-1);
        if (kind < 0 || kind > int(LayerOp::Kind::PopPath)) {
            if (error) *error = QStringLiteral("An op of an unknown kind");
            return std::nullopt;
        }
        op.kind = LayerOp::Kind(kind);
        op.path = path(entry[1]);
        op.otherPath = path(entry[2]);
        op.field = TfToken(entry[3].toString().toStdString());
        op.token = TfToken(entry[4].toString().toStdString());
        op.time = entry[5].toString().toDouble();
        op.specType = SdfSpecType(entry[6].toInt());
        op.inert = entry[7].toBool();
        const QString ref = entry[8].toString();
        const QString rest = ref.mid(1);
        const QChar tag = ref.isEmpty() ? QLatin1Char('-') : ref[0];
        if (tag == QLatin1Char('k')) {
            op.value = VtValue(TfToken(rest.toStdString()));
        } else if (tag == QLatin1Char('t')) {
            TfTokenVector tokens;
            if (!rest.isEmpty()) {
                for (const QString& t : rest.split(QLatin1Char('\n'))) tokens.emplace_back(t.toStdString());
            }
            op.value = VtValue(tokens);
        } else if (tag == QLatin1Char('p')) {
            SdfPathVector paths;
            if (!rest.isEmpty()) {
                for (const QString& p : rest.split(QLatin1Char('\n'))) paths.emplace_back(p.toStdString());
            }
            op.value = VtValue(paths);
        } else if (tag == QLatin1Char('s')) {
            bool ok = false;
            const int slot = rest.toInt(&ok);
            if (!ok || slot < 0 || slot >= int(scratch.size())) {
                if (error) *error = QStringLiteral("An op's value is missing");
                return std::nullopt;
            }
            const SdfLayerHandle layer = scratch[size_t(slot)];
            if (op.kind == LayerOp::Kind::SetField) {
                op.value = layer->GetField(op.path, op.field);
            } else if (op.kind == LayerOp::Kind::SetDictValue) {
                op.value = layer->GetFieldDictValueByKey(op.path, op.field, op.token);
            } else {
                layer->QueryTimeSample(op.path, op.time, &op.value);
            }
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

std::optional<QByteArray> layerToUsdc(const SdfLayerHandle& layer, QString* error) {
    const QString name = temporaryUsdcPath();
    if (!layer->Export(QDir::toNativeSeparators(name).toStdString())) {
        if (error) *error = QStringLiteral("Can't write the layer as USDC to %1").arg(name);
        return std::nullopt;
    }
    QFile in(name);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Can't read back %1").arg(name);
        return std::nullopt;
    }
    QByteArray bytes = in.readAll();
    in.close();
    QFile::remove(name);
    return bytes;
}

bool usdcToLayer(const QByteArray& bytes, const SdfLayerHandle& layer, QString* error) {
    const QString name = temporaryUsdcPath();
    {
        QFile file(name);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            if (error) *error = QStringLiteral("Can't write %1: %2").arg(name, file.errorString());
            return false;
        }
    }
    bool ok = false;
    {
        SdfLayerRefPtr opened = SdfLayer::OpenAsAnonymous(QDir::toNativeSeparators(name).toStdString());
        if (opened) {
            layer->TransferContent(opened);
            ok = true;
        } else if (error) {
            *error = QStringLiteral("Can't read the USDC that arrived");
        }
    }
    QFile::remove(name);  // may still be mapped; the folder goes when the process ends
    return ok;
}

QByteArray packChunks(const std::vector<QByteArray>& chunks) {
    QByteArray packed;
    QDataStream out(&packed, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << quint32(chunks.size());
    for (const QByteArray& chunk : chunks) out << chunk;
    return packed;
}

std::optional<std::vector<QByteArray>> unpackChunks(const QByteArray& packed) {
    std::vector<QByteArray> chunks;
    if (packed.isEmpty()) return chunks;
    QDataStream in(packed);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 count = 0;
    in >> count;
    if (count > 1024) return std::nullopt;
    for (quint32 i = 0; i < count; ++i) {
        QByteArray chunk;
        in >> chunk;
        if (in.status() != QDataStream::Ok) return std::nullopt;
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

QString layerDigest(const SdfLayerHandle& layer) {
    std::string text;
    layer->ExportToString(&text);
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex());
}

}  // namespace usdcc::usd::share
