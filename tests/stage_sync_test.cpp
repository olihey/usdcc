// docs/SHARED_STAGE.md §11: peers in one process, their live layers joined by a pipe
// that orders like a room's host and delays at random. Random edits from every side
// must end in identical layers; and the snapshot, undo, a deleted spec, a view-only
// member, a drag, large batches and the check.
#include "test_harness.h"

#include "usdcc/usd/share/stage_sync.h"
#include "usdcc/usd/stage.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <QFile>

#include <algorithm>
#include <deque>
#include <memory>
#include <random>

using namespace PXR_NS;
using usdcc::peers::Message;
using usdcc::peers::PeerId;
using usdcc::usd::UndoManager;
using namespace usdcc::usd::share;

namespace {

std::string firstDifference(const SdfLayerHandle& a, const SdfLayerHandle& b) {
    std::string as, bs;
    a->ExportToString(&as);
    b->ExportToString(&bs);
    if (as == bs) return {};
    size_t i = 0;
    while (i < as.size() && i < bs.size() && as[i] == bs[i]) ++i;
    const size_t from = i > 80 ? i - 80 : 0;
    return "...\n" + as.substr(from, 200) + "\n   vs\n" + bs.substr(from, 200);
}

// A room's host as far as a shared stage needs one: it numbers ordered messages and
// passes them to everyone, the sender too, each member's in the order they were sent.
// Node 0 is the host, whose own messages it routes at once, as Room does.
class Pipe {
public:
    struct Node {
        PeerId id;
        SdfLayerRefPtr live;
        UsdStageRefPtr stage;
        std::unique_ptr<StageSync> sync;
        std::deque<Message> up;    // to the host
        std::deque<Message> down;  // from the host
        bool viewOnly = false;
        bool editing = false;
        bool connected = true;
    };

    explicit Pipe(unsigned seed, StageSyncOptions base = {}) : m_rng(seed), m_base(std::move(base)) {}

    Node& add(bool viewOnly = false) {
        auto node = std::make_unique<Node>();
        Node& n = *node;
        const int index = int(m_nodes.size());
        n.id = QStringLiteral("peer%1").arg(index);
        n.viewOnly = viewOnly;
        n.live = SdfLayer::CreateAnonymous("live.usda");
        n.stage = UsdStage::Open(n.live);
        StageSyncOptions options = m_base;
        options.live = n.live;
        options.me = n.id;
        options.host = index == 0;
        options.timers = false;
        options.sendOrdered = [this, index](const QString& type, const QJsonObject& body, const QByteArray& bytes) {
            Message m = message(index, type, body, bytes);
            if (index == 0) {
                route(m);
            } else {
                m_nodes[size_t(index)]->up.push_back(m);
            }
        };
        options.sendLatest = [this, index](const QString& type, const QJsonObject& body, const QByteArray& bytes) {
            const Message m = message(index, type, body, bytes);
            for (size_t i = 0; i < m_nodes.size(); ++i) {
                if (int(i) != index) m_nodes[i]->down.push_back(m);
            }
        };
        options.sendDirect = [this, index](const PeerId& to, const QString& type, const QJsonObject& body,
                                           const QByteArray& bytes) {
            Message m = message(index, type, body, bytes);
            m.to = to.isEmpty() ? m_nodes[0]->id : to;
            if (index == 0) {
                nodeById(m.to).down.push_back(m);
            } else {
                m_nodes[size_t(index)]->up.push_back(m);
            }
        };
        options.sendFile = [this](const PeerId& to, const QString& path, const QString& id) {
            Message m;
            m.type = QStringLiteral("file");
            m.body = {{QStringLiteral("id"), id}, {QStringLiteral("path"), path}};
            nodeById(to).down.push_back(m);
        };
        options.editing = [&n] { return n.editing; };
        n.sync = std::make_unique<StageSync>(options);
        if (index > 0) {
            // The welcome: the snapshot first, then what follows.
            QByteArray bytes;
            Message snapshot;
            snapshot.type = QStringLiteral("snapshot");
            snapshot.body = m_nodes[0]->sync->snapshotBody(&bytes);
            snapshot.bytes = bytes;
            n.down.push_back(snapshot);
        }
        m_nodes.push_back(std::move(node));
        return n;
    }

    Node& node(size_t i) { return *m_nodes[i]; }
    size_t size() const { return m_nodes.size(); }

    // Delivers one waiting message, chosen at random; false if none waits.
    bool step() {
        std::vector<std::deque<Message>*> queues;
        for (auto& n : m_nodes) {
            if (!n->connected) continue;
            if (!n->up.empty()) queues.push_back(&n->up);
            if (!n->down.empty()) queues.push_back(&n->down);
        }
        if (queues.empty()) return false;
        std::deque<Message>* queue = queues[std::uniform_int_distribution<size_t>(0, queues.size() - 1)(m_rng)];
        const Message m = queue->front();
        queue->pop_front();
        for (auto& n : m_nodes) {
            if (queue == &n->up) {
                route(m);
                return true;
            }
            if (queue == &n->down) {
                deliver(*n, m);
                return true;
            }
        }
        return true;
    }

    void drain() {
        for (int i = 0; i < 1000000 && step(); ++i) {
        }
    }

    std::mt19937& rng() { return m_rng; }

private:
    Message message(int from, const QString& type, const QJsonObject& body, const QByteArray& bytes) {
        Message m;
        m.from = m_nodes[size_t(from)]->id;
        m.type = type;
        m.body = body;
        m.bytes = bytes;
        return m;
    }

    Node& nodeById(const PeerId& id) {
        for (auto& n : m_nodes) {
            if (n->id == id) return *n;
        }
        return *m_nodes[0];
    }

    void route(Message m) {
        if (!m.to.isEmpty()) {  // direct
            Node& to = nodeById(m.to);
            if (&to == m_nodes[0].get()) {
                deliver(to, m);
            } else {
                to.down.push_back(m);
            }
            return;
        }
        Node& from = nodeById(m.from);
        if (from.viewOnly) {
            from.sync->refused(m.type, QStringLiteral("This member may only watch"));
            return;
        }
        m.seq = ++m_seq;
        for (auto& n : m_nodes) {
            if (n.get() == m_nodes[0].get()) {
                deliver(*n, m);
            } else {
                n->down.push_back(m);
            }
        }
    }

    void deliver(Node& n, const Message& m) {
        if (m.type == QLatin1String("file")) {
            n.sync->fileArrived(m.body.value(QLatin1String("id")).toString(),
                                m.body.value(QLatin1String("path")).toString());
            return;
        }
        n.sync->receive(m);
    }

    std::mt19937 m_rng;
    StageSyncOptions m_base;
    std::vector<std::unique_ptr<Node>> m_nodes;
    qint64 m_seq = 0;
};

void checkAllEqual(Pipe& pipe, const char* what) {
    const SdfLayerHandle truth = pipe.node(0).sync->confirmed();
    for (size_t i = 0; i < pipe.size(); ++i) {
        Pipe::Node& n = pipe.node(i);
        CHECK(n.sync->pendingBatches() == 0, "%s: %s has nothing left to send (%d)", what, n.id.toUtf8().constData(),
              n.sync->pendingBatches());
        const std::string live = firstDifference(truth, n.live);
        CHECK(live.empty(), "%s: %s's live layer is the host's: %s", what, n.id.toUtf8().constData(), live.c_str());
        const std::string confirmed = firstDifference(truth, n.sync->confirmed());
        CHECK(confirmed.empty(), "%s: %s's confirmed layer is the host's: %s", what, n.id.toUtf8().constData(),
              confirmed.c_str());
    }
}

// One random edit through the node's stage, as a panel or a tool would make it.
void randomEdit(Pipe& pipe, Pipe::Node& n) {
    std::mt19937& rng = pipe.rng();
    const int k = std::uniform_int_distribution<int>(0, 5)(rng);
    const SdfPath path(("/World/P" + std::to_string(k)).c_str());
    switch (std::uniform_int_distribution<int>(0, 6)(rng)) {
    case 0:
    case 1: {
        UsdGeomXform::Define(n.stage, SdfPath("/World"));
        n.stage->DefinePrim(path, TfToken("Xform"))
            .CreateAttribute(TfToken("x"), SdfValueTypeNames->Float)
            .Set(float(std::uniform_int_distribution<int>(0, 1000)(rng)));
        break;
    }
    case 2:
        if (UsdPrim prim = n.stage->GetPrimAtPath(path)) {
            prim.CreateAttribute(TfToken("y"), SdfValueTypeNames->Double)
                .Set(double(std::uniform_int_distribution<int>(0, 1000)(rng)), UsdTimeCode(k));
        }
        break;
    case 3: n.stage->RemovePrim(path); break;
    case 4:
        if (UsdPrim prim = n.stage->GetPrimAtPath(path)) {
            prim.SetCustomDataByKey(TfToken("note"), VtValue(std::string(n.id.toStdString())));
        }
        break;
    case 5:
        if (UsdPrim prim = n.stage->GetPrimAtPath(path)) {
            prim.SetActive(std::uniform_int_distribution<int>(0, 1)(rng) == 1);
        }
        break;
    case 6:
        if (UsdPrim prim = n.stage->GetPrimAtPath(path); prim && prim.IsActive()) {
            n.stage->DefinePrim(path.AppendChild(TfToken("Child")), TfToken("Scope"));
        }
        break;
    }
}

}  // namespace

USDCC_TEST(stage_sync_random_edits_converge) {
    for (unsigned seed = 1; seed <= 30; ++seed) {
        Pipe pipe(seed);
        pipe.add();
        pipe.add();
        pipe.add();
        for (int i = 0; i < 300; ++i) {
            const int what = std::uniform_int_distribution<int>(0, 9)(pipe.rng());
            if (what < 3) {
                Pipe::Node& n = pipe.node(std::uniform_int_distribution<size_t>(0, pipe.size() - 1)(pipe.rng()));
                if (n.sync->isSynced()) {
                    randomEdit(pipe, n);
                    if (what == 0) n.sync->flush();
                }
            } else if (what < 5) {
                pipe.node(std::uniform_int_distribution<size_t>(0, pipe.size() - 1)(pipe.rng())).sync->flush();
            } else {
                pipe.step();
            }
            if (seed % 5 == 0 && i == 150) pipe.add();  // a late joiner, mid-stream
        }
        for (size_t i = 0; i < pipe.size(); ++i) pipe.node(i).sync->flush();
        pipe.drain();
        const std::string what = "seed " + std::to_string(seed);
        checkAllEqual(pipe, what.c_str());
        if (::usdcc::test::failures() > 0) break;  // one seed's report is enough
    }
}

USDCC_TEST(stage_sync_same_field_last_writer_wins) {
    Pipe pipe(7);
    Pipe::Node& a = pipe.add();
    Pipe::Node& b = pipe.add();
    Pipe::Node& c = pipe.add();
    pipe.drain();
    UsdGeomXform::Define(a.stage, SdfPath("/Box"));
    a.sync->flush();
    pipe.drain();
    // B and C both move the box before either hears of the other.
    b.stage->GetPrimAtPath(SdfPath("/Box")).CreateAttribute(TfToken("x"), SdfValueTypeNames->Float).Set(2.f);
    c.stage->GetPrimAtPath(SdfPath("/Box")).CreateAttribute(TfToken("x"), SdfValueTypeNames->Float).Set(3.f);
    b.sync->flush();
    c.sync->flush();
    pipe.drain();
    checkAllEqual(pipe, "same field");
    float x = 0;
    a.stage->GetPrimAtPath(SdfPath("/Box")).GetAttribute(TfToken("x")).Get(&x);
    CHECK(x == 2.f || x == 3.f, "one of the two values stays (%g)", double(x));
}

USDCC_TEST(stage_sync_late_joiner_and_large_snapshot) {
    StageSyncOptions small;
    small.inlineSnapshotBytes = 2048;  // anything bigger goes as a file
    small.partBytes = 4096;            // and batches in many parts
    Pipe pipe(11, small);
    Pipe::Node& host = pipe.add();
    Pipe::Node& b = pipe.add();
    pipe.drain();
    VtFloatArray big(20000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = float(i) * 0.25f;
    host.stage->DefinePrim(SdfPath("/Big")).CreateAttribute(TfToken("v"), SdfValueTypeNames->FloatArray).Set(big);
    host.sync->flush();
    b.stage->DefinePrim(SdfPath("/FromB")).SetCustomDataByKey(TfToken("k"), VtValue(1));
    b.sync->flush();
    pipe.drain();
    Pipe::Node& late = pipe.add();  // gets a snapshot of more than 2 KB: as a file
    pipe.drain();
    CHECK(late.sync->isSynced(), "the late joiner is in");
    checkAllEqual(pipe, "late joiner");
    VtFloatArray got;
    late.stage->GetPrimAtPath(SdfPath("/Big")).GetAttribute(TfToken("v")).Get(&got);
    CHECK(got == big, "the large array arrived whole");
}

USDCC_TEST(stage_sync_undo_takes_back_only_its_own_step) {
    Pipe pipe(3);
    Pipe::Node& a = pipe.add();
    Pipe::Node& b = pipe.add();
    pipe.drain();
    UndoManager undo;
    auto stageA = std::make_shared<usdcc::usd::Stage>(a.stage);
    UsdGeomXform::Define(a.stage, SdfPath("/Box"));
    a.sync->flush();
    pipe.drain();

    undo.apply(stageA, QStringLiteral("Set X"), [&] {
        return a.stage->GetPrimAtPath(SdfPath("/Box")).CreateAttribute(TfToken("x"), SdfValueTypeNames->Float).Set(1.f);
    });
    a.sync->flush();
    b.stage->GetPrimAtPath(SdfPath("/Box")).CreateAttribute(TfToken("y"), SdfValueTypeNames->Float).Set(2.f);
    b.sync->flush();
    pipe.drain();
    undo.stack(stageA)->undo();
    a.sync->flush();
    pipe.drain();
    checkAllEqual(pipe, "undo");
    const UsdPrim box = b.stage->GetPrimAtPath(SdfPath("/Box"));
    CHECK(!box.HasAttribute(TfToken("x")), "A's step is undone on B too");
    CHECK(box.HasAttribute(TfToken("y")), "B's change stays");

    // An inverse for a spec another peer deleted is dropped, not an error.
    undo.apply(stageA, QStringLiteral("Make Gone"), [&] {
        return bool(a.stage->DefinePrim(SdfPath("/Box/Gone")).CreateAttribute(TfToken("z"), SdfValueTypeNames->Int));
    });
    a.sync->flush();
    pipe.drain();
    b.stage->RemovePrim(SdfPath("/Box/Gone"));
    b.sync->flush();
    pipe.drain();
    undo.stack(stageA)->undo();
    a.sync->flush();
    pipe.drain();
    checkAllEqual(pipe, "undo after a delete");
    undo.removeStage(stageA);
}

USDCC_TEST(stage_sync_view_only_member_is_refused) {
    Pipe pipe(5);
    Pipe::Node& host = pipe.add();
    Pipe::Node& watcher = pipe.add(true);
    pipe.drain();
    UsdGeomXform::Define(host.stage, SdfPath("/Box"));
    host.sync->flush();
    pipe.drain();
    QString reason;
    QObject::connect(watcher.sync.get(), &StageSync::editsRefused, [&](const QString& r) { reason = r; });
    watcher.stage->DefinePrim(SdfPath("/Sneaky"));
    watcher.sync->flush();
    pipe.drain();
    CHECK(!reason.isEmpty(), "the watcher hears its edit was refused");
    CHECK(!watcher.stage->GetPrimAtPath(SdfPath("/Sneaky")), "and it is taken back");
    checkAllEqual(pipe, "view only");
}

USDCC_TEST(stage_sync_drag_previews_then_one_batch) {
    Pipe pipe(9);
    Pipe::Node& a = pipe.add();
    Pipe::Node& b = pipe.add();
    pipe.drain();
    UsdAttribute x = a.stage->DefinePrim(SdfPath("/Box")).CreateAttribute(TfToken("x"), SdfValueTypeNames->Float);
    a.sync->flush();
    pipe.drain();
    a.editing = true;
    for (int i = 1; i <= 50; ++i) x.Set(float(i));
    a.sync->flush();  // held: the drag isn't over
    CHECK(a.sync->pendingBatches() == 1, "the drag's changes wait");
    CHECK(a.up.empty() && b.down.empty(), "and nothing ordered is sent");
    // What a timer does while dragging: the values so far, latest only.
    a.sync->sendPreview();
    pipe.drain();
    float seen = 0;
    b.stage->GetPrimAtPath(SdfPath("/Box")).GetAttribute(TfToken("x")).Get(&seen);
    CHECK(seen == 50.f, "B sees the drag move (%g)", double(seen));
    a.editing = false;
    a.sync->flush();
    pipe.drain();
    checkAllEqual(pipe, "drag");
}

USDCC_TEST(stage_sync_check_finds_a_difference) {
    Pipe pipe(13);
    Pipe::Node& host = pipe.add();
    Pipe::Node& b = pipe.add();
    pipe.drain();
    host.stage->DefinePrim(SdfPath("/Box"));
    host.sync->flush();
    pipe.drain();
    // Something went wrong on B: its layers no longer match.
    applyOps(b.sync->confirmed(), {setFieldOp(b.sync->confirmed(), SdfPath("/Box"), SdfFieldKeys->Documentation,
                                              VtValue(std::string("drift")))});
    bool mismatched = false;
    QObject::connect(b.sync.get(), &StageSync::mismatched, [&] { mismatched = true; });
    host.sync->sendCheck();
    pipe.drain();
    CHECK(mismatched, "B notices");
    checkAllEqual(pipe, "after the check");
}

USDCC_TEST(stage_sync_layer_ops_round_trip) {
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("a.usda");
    LayerOps ops;
    UndoManager::observeLayer(layer, [&](const LayerOp& op) { ops.push_back(op); });
    UsdStageRefPtr stage = UsdStage::Open(layer);
    UsdPrim prim = stage->DefinePrim(SdfPath("/A"), TfToken("Xform"));
    prim.CreateAttribute(TfToken("f"), SdfValueTypeNames->Float3).Set(GfVec3f(1, 2, 3));
    prim.CreateRelationship(TfToken("r")).AddTarget(SdfPath("/A"));
    UndoManager::observeLayer(layer, {});
    QString error;
    std::optional<EncodedOps> encoded = encodeOps(ops, &error);
    CHECK(encoded.has_value(), "the ops encode: %s", error.toUtf8().constData());
    if (!encoded) return;
    std::optional<LayerOps> decoded = decodeOps(*encoded, &error);
    CHECK(decoded.has_value(), "and decode: %s", error.toUtf8().constData());
    if (!decoded) return;
    SdfLayerRefPtr copy = SdfLayer::CreateAnonymous("b.usda");
    CHECK(applyOps(copy, *decoded) == 0, "every op fits");
    const std::string diff = firstDifference(layer, copy);
    CHECK(diff.empty(), "the copy is equal: %s", diff.c_str());
}
