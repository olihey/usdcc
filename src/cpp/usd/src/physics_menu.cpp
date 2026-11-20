#include "usdcc/usd/physics_menu.h"

#include "usdcc/core/log.h"
#include "usdcc/core/settings.h"
#include "usdcc/usd/physics_check.h"
#include "usdcc/usd/physics_session.h"
#include "usdcc/usd/playback.h"
#include "usdcc/usd/playback_controller.h"
#include "usdcc/usd/stage_manager.h"
#include "usdcc/usd/time_ruler.h"
#include "usdcc/usd/undo_manager.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdPhysics/collisionAPI.h>
#include <pxr/usd/usdPhysics/meshCollisionAPI.h>
#include <pxr/usd/usdPhysics/rigidBodyAPI.h>
#include <pxr/usd/usdPhysics/scene.h>
#include <pxr/usd/usdPhysics/tokens.h>

#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::usd {

namespace physics_actions {

namespace {

// The gprims at or under `prim`, outside instances.
std::vector<UsdPrim> geometryOf(const UsdPrim& prim) {
    std::vector<UsdPrim> found;
    for (const UsdPrim& each : UsdPrimRange(prim)) {
        if (each.IsInstanceProxy()) continue;
        if (each.IsA<UsdGeomGprim>()) found.push_back(each);
    }
    return found;
}

// The prims that can be edited: there, not inside an instance, reachable by the edit target.
std::vector<UsdPrim> editable(const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    std::vector<UsdPrim> prims;
    for (const SdfPath& path : paths) {
        const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
        if (prim && !prim.IsInstanceProxy() && !prim.IsPseudoRoot() && editTargetReaches(prim)) prims.push_back(prim);
    }
    return prims;
}

void addColliders(const UsdPrim& prim, bool moving) {
    for (const UsdPrim& geometry : geometryOf(prim)) {
        if (!geometry.HasAPI<UsdPhysicsCollisionAPI>()) UsdPhysicsCollisionAPI::Apply(geometry);
        if (moving && geometry.IsA<UsdGeomMesh>()) {
            UsdPhysicsMeshCollisionAPI api = UsdPhysicsMeshCollisionAPI::Apply(geometry);
            if (!api.GetApproximationAttr().HasAuthoredValue()) api.CreateApproximationAttr().Set(UsdPhysicsTokens->convexHull);
        }
    }
}

// A free name for a prim under the root: `base`, then base1, base2, ...
SdfPath freePath(const UsdStageRefPtr& stage, const std::string& base) {
    SdfPath path = SdfPath::AbsoluteRootPath().AppendChild(TfToken(base));
    for (int i = 1; stage->GetPrimAtPath(path); ++i) {
        path = SdfPath::AbsoluteRootPath().AppendChild(TfToken(base + std::to_string(i)));
    }
    return path;
}

}  // namespace

bool makeRigidBody(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    if (!stage) return false;
    const std::vector<UsdPrim> prims = editable(stage, paths);
    if (prims.empty()) return false;
    return undo.apply(stage, QObject::tr("Make Rigid Body"), [&prims]() {
        for (const UsdPrim& prim : prims) {
            if (!prim.HasAPI<UsdPhysicsRigidBodyAPI>()) UsdPhysicsRigidBodyAPI::Apply(prim);
            addColliders(prim, true);
        }
        return true;
    });
}

bool makeStaticCollider(UndoManager& undo, const StageRefPtr& stage, const std::vector<SdfPath>& paths) {
    if (!stage) return false;
    const std::vector<UsdPrim> prims = editable(stage, paths);
    if (prims.empty()) return false;
    return undo.apply(stage, QObject::tr("Make Static Collider"), [&prims]() {
        for (const UsdPrim& prim : prims) addColliders(prim, false);
        return true;
    });
}

SdfPath addGroundPlane(UndoManager& undo, const StageRefPtr& stage) {
    if (!stage) return {};
    const UsdStageRefPtr& usd = stage->usdStage();
    const SdfPath path = freePath(usd, "GroundPlane");
    const double size = 20.0 / UsdGeomGetStageMetersPerUnit(usd);  // 20 m across
    const bool done = undo.apply(stage, QObject::tr("Add Ground Plane"), [&]() {
        UsdGeomPlane plane = UsdGeomPlane::Define(usd, path);
        if (!plane) return false;
        plane.CreateAxisAttr().Set(UsdGeomGetStageUpAxis(usd));
        plane.CreateWidthAttr().Set(size);
        plane.CreateLengthAttr().Set(size);
        UsdPhysicsCollisionAPI::Apply(plane.GetPrim());
        return true;
    });
    if (!done) return {};
    stage->primCreated(path);
    return path;
}

SdfPath addPhysicsScene(UndoManager& undo, const StageRefPtr& stage) {
    if (!stage) return {};
    const UsdStageRefPtr& usd = stage->usdStage();
    const SdfPath path = freePath(usd, "PhysicsScene");
    const bool done = undo.apply(stage, QObject::tr("Add Physics Scene"),
                                 [&]() { return static_cast<bool>(UsdPhysicsScene::Define(usd, path)); });
    if (!done) return {};
    stage->primCreated(path);
    return path;
}

physics::BakeResult bakeSimulation(UndoManager& undo, const StageRefPtr& stage, const physics::BakeOptions& options,
                                   const std::function<bool(int, int)>& progress) {
    physics::BakeResult result;
    if (!stage) return result;
    physics::PhysicsSession& session = stage->physics();
    session.setSubsteps(usdcc::core::Settings::instance().get<int>("Physics/substeps", 4));
    if (!physics::cacheRange(session, playbackFrames(stage->playbackRange()), progress)) {
        result.reports = session.reports();
        return result;
    }
    undo.apply(stage, QObject::tr("Bake Simulation"), [&]() {
        result = physics::bakeFromCache(stage->usdStage(), session, options);
        return !result.baked.empty();
    });
    // The bake shows only once the cache, which is stronger, has gone.
    if (!result.baked.empty()) session.reset();
    return result;
}

}  // namespace physics_actions

namespace {

QString primLabel(const SdfPath& path) {
    return path.IsAbsoluteRootPath() ? QObject::tr("(the stage)") : QString::fromStdString(path.GetString());
}

}  // namespace

PhysicsMenu::PhysicsMenu(StageManager* stageManager, std::shared_ptr<UndoManager> undo, PlaybackController* playback,
                         QWidget* parent)
    : QMenu(tr("&Physics"), parent), m_stageManager(stageManager), m_undo(std::move(undo)), m_playback(playback) {
    setObjectName(QStringLiteral("physicsMenu"));
    setToolTipsVisible(true);
    connect(this, &QMenu::aboutToShow, this, &PhysicsMenu::rebuild);
    rebuild();
}

void PhysicsMenu::rebuild() {
    clear();
    const StageRefPtr stage = m_stageManager->currentStage();
    const std::vector<SdfPath> selection = stage ? m_stageManager->selectedPaths(stage) : std::vector<SdfPath>{};
    const bool canEdit = stage && m_undo;

    // Makes a prim and selects it, as one undo step.
    auto select = [this, stage](const std::function<SdfPath()>& make) {
        const UndoManager::SelectionScope selection(*m_undo, stage);
        const SdfPath path = make();
        if (!path.IsEmpty()) m_stageManager->setSelectedPaths(stage, {path});
    };
    QAction* body = addAction(tr("Make Rigid Body"), this, [this, stage, selection]() {
        physics_actions::makeRigidBody(*m_undo, stage, selection);
    });
    body->setObjectName(QStringLiteral("physicsMakeRigidBody"));
    body->setToolTip(tr("The selection moves under gravity and collides: RigidBodyAPI on it, CollisionAPI on its "
                        "geometry, and a convex hull for its meshes"));
    body->setEnabled(canEdit && !selection.empty());
    QAction* collider = addAction(tr("Make Static Collider"), this, [this, stage, selection]() {
        physics_actions::makeStaticCollider(*m_undo, stage, selection);
    });
    collider->setObjectName(QStringLiteral("physicsMakeStaticCollider"));
    collider->setToolTip(tr("The selection stays where it is, and bodies land on it: CollisionAPI on its geometry"));
    collider->setEnabled(canEdit && !selection.empty());
    QAction* ground = addAction(tr("Add Ground Plane"), this,
                                [this, stage, select]() { select([&]() { return physics_actions::addGroundPlane(*m_undo, stage); }); });
    ground->setObjectName(QStringLiteral("physicsAddGroundPlane"));
    ground->setToolTip(tr("A plane at the up axis' zero that bodies land on"));
    ground->setEnabled(canEdit);
    QAction* scene = addAction(tr("Add Physics Scene"), this,
                               [this, stage, select]() { select([&]() { return physics_actions::addPhysicsScene(*m_undo, stage); }); });
    scene->setObjectName(QStringLiteral("physicsAddScene"));
    scene->setToolTip(tr("Gravity for the stage's simulation; without one, it uses Earth's down the up axis"));
    scene->setEnabled(canEdit);

    addSeparator();
    QAction* check = addAction(tr("Check Physics..."), this, &PhysicsMenu::checkInteractively);
    check->setObjectName(QStringLiteral("physicsCheck"));
    check->setToolTip(tr("What OpenUSD's physics validators say about the stage, and what a simulation of it would "
                         "leave out or change"));
    check->setEnabled(stage != nullptr);

    addSeparator();
    QAction* simulate = addAction(tr("Simulate"));
    simulate->setObjectName(QStringLiteral("physicsSimulate"));
    simulate->setCheckable(true);
    simulate->setChecked(m_playback && m_playback->simulates());
    simulate->setEnabled(m_playback != nullptr);
    simulate->setToolTip(tr("Playing the timeline solves the stage's physics, frame by frame"));
    connect(simulate, &QAction::triggered, this, [this](bool on) {
        if (m_playback) m_playback->setSimulates(on);
    });
    QAction* reset = addAction(tr("Reset Simulation"), this, [this]() {
        if (m_playback) m_playback->resetSimulation();
    });
    reset->setObjectName(QStringLiteral("physicsResetSimulation"));
    reset->setToolTip(tr("Clear the cached frames and go to the first frame"));
    physics::PhysicsSession* session = stage ? stage->physicsIfStarted() : nullptr;
    reset->setEnabled(m_playback && session && session->cachedRange());
    QAction* bake = addAction(tr("Bake Simulation..."), this, &PhysicsMenu::bakeInteractively);
    bake->setObjectName(QStringLiteral("physicsBake"));
    bake->setToolTip(tr("Keep the simulation: its frames as time samples on the bodies' transforms, in the edit "
                        "target, one undo step"));
    bake->setEnabled(canEdit);
}

void PhysicsMenu::bakeInteractively() {
    const StageRefPtr stage = m_stageManager->currentStage();
    if (!stage || !m_undo) return;
    if (m_playback) m_playback->stop();
    const std::vector<SdfPath> selection = m_stageManager->selectedPaths(stage);
    const TimeRange frames = playbackFrames(stage->playbackRange());

    QDialog dialog(parentWidget());
    dialog.setWindowTitle(tr("Bake Simulation"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* all = new QRadioButton(tr("All rigid bodies"), &dialog);
    auto* selected = new QRadioButton(tr("Rigid bodies in the selection"), &dialog);
    all->setObjectName(QStringLiteral("bakeAll"));
    selected->setObjectName(QStringLiteral("bakeSelected"));
    selected->setEnabled(!selection.empty());
    (selection.empty() ? all : selected)->setChecked(true);
    auto* kinematic = new QCheckBox(tr("Mark baked bodies kinematic"), &dialog);
    kinematic->setObjectName(QStringLiteral("bakeMarkKinematic"));
    kinematic->setToolTip(tr("A later simulation then moves them as baked, and other bodies collide with them, "
                             "instead of solving them again"));
    auto* note = new QLabel(tr("Frames %1 to %2: the playback range. Frames that aren't cached yet are solved "
                               "first. The frames go into the edit target, as one undo step.")
                                .arg(formatFrame(frames.start), formatFrame(frames.end)),
                            &dialog);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("propertyNote"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Bake"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    for (QWidget* widget : std::initializer_list<QWidget*>{all, selected, kinematic, note}) layout->addWidget(widget);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    physics::BakeOptions options;
    if (selected->isChecked()) options.prims = selection;
    options.markKinematic = kinematic->isChecked();
    QProgressDialog progress(tr("Solving the frames to bake..."), tr("Cancel"), 0, std::max(1, int(frames.end - frames.start)),
                             parentWidget());
    progress.setWindowTitle(tr("Bake Simulation"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    const physics::BakeResult result =
        physics_actions::bakeSimulation(*m_undo, stage, options, [&progress](int done, int total) {
            progress.setMaximum(std::max(1, total));
            progress.setValue(done);
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        });
    progress.reset();
    if (progress.wasCanceled()) return;
    QStringList problems;
    for (const physics::PhysicsReport& report : result.reports) {
        problems << QStringLiteral("%1: %2").arg(primLabel(report.prim), QString::fromStdString(report.message));
    }
    if (result.baked.empty()) {
        QMessageBox::information(parentWidget(), tr("Bake Simulation"),
                                 problems.isEmpty() ? tr("There's no rigid body to bake.")
                                                    : tr("Nothing was baked.\n\n%1").arg(problems.join(QLatin1Char('\n'))));
        return;
    }
    USDCC_LOG_INFO("usdcc.physics", "Baked {} bodies, frames {} to {}", result.baked.size(), result.frames->start,
                   result.frames->end);
    if (!problems.isEmpty()) {
        QMessageBox::warning(parentWidget(), tr("Bake Simulation"),
                             tr("Baked %n body(ies), but:\n\n%1", nullptr, int(result.baked.size())).arg(problems.join(QLatin1Char('\n'))));
    }
}

void PhysicsMenu::checkInteractively() {
    const StageRefPtr stage = m_stageManager->currentStage();
    if (!stage) return;
    const std::vector<physics::CheckFinding> findings = physics::checkPhysics(stage->usdStage(), stage->time());

    auto* dialog = new QDialog(parentWidget());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("checkPhysicsDialog"));
    dialog->setWindowTitle(tr("Check Physics"));
    dialog->resize(760, 420);
    auto* layout = new QVBoxLayout(dialog);
    auto* summary = new QLabel(findings.empty() ? tr("No problems found: the validators are content, and a simulation "
                                                     "would take the stage as it is.")
                                                : tr("%n finding(s). Double-click one to select its prim.", nullptr,
                                                     int(findings.size())),
                               dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* list = new QTreeWidget(dialog);
    list->setObjectName(QStringLiteral("checkPhysicsList"));
    list->setRootIsDecorated(false);
    list->setHeaderLabels({tr("Prim"), tr("Finding"), tr("From")});
    for (const physics::CheckFinding& finding : findings) {
        auto* item = new QTreeWidgetItem(list);
        item->setText(0, primLabel(finding.prim));
        item->setText(1, QString::fromStdString(finding.message));
        item->setToolTip(1, item->text(1));
        item->setText(2, finding.source == physics::CheckFinding::Source::Validator
                             ? (finding.error ? tr("Validator: error") : tr("Validator"))
                             : tr("Simulation"));
        item->setData(0, Qt::UserRole, QString::fromStdString(finding.prim.GetString()));
    }
    list->resizeColumnToContents(0);
    list->setColumnWidth(1, 440);
    list->setVisible(!findings.empty());
    layout->addWidget(list, 1);
    std::weak_ptr<Stage> weak = stage;
    connect(list, &QTreeWidget::itemDoubleClicked, dialog, [this, weak](QTreeWidgetItem* item) {
        const StageRefPtr locked = weak.lock();
        const SdfPath path(item->data(0, Qt::UserRole).toString().toStdString());
        if (locked && !path.IsAbsoluteRootPath() && locked->usdStage()->GetPrimAtPath(path)) {
            m_stageManager->setSelectedPaths(locked, {path});
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

}  // namespace usdcc::usd
