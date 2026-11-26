#include "usdcc/usd/primvar_views.h"

#include "usdcc/usd/stage_manager.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace usdcc::usd {

using namespace PXR_NS;

namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }
QString html(const std::string& text) { return qs(text).toHtmlEscaped(); }

QString number(std::size_t n) { return QLocale().toString(qulonglong(n)); }
QString number(double n) { return QLocale().toString(n, 'g', 5); }

QString layerName(const std::string& identifier) {
    return identifier.empty() ? QObject::tr("no layer") : qs(SdfLayer::GetDisplayNameFromIdentifier(identifier));
}

// As the log's and the Validation panel's.
const QColor kSeverityColors[] = {QColor(0xff, 0x5f, 0x56), QColor(0xff, 0xbd, 0x2e), QColor(0x6c, 0xb6, 0xff)};

QString severityWord(PrimvarProblem::Severity severity) {
    switch (severity) {
        case PrimvarProblem::Severity::Error: return QObject::tr("Error");
        case PrimvarProblem::Severity::Warning: return QObject::tr("Warning");
        case PrimvarProblem::Severity::Info: return QObject::tr("Info");
    }
    return {};
}

// How long edits are gathered before the prims are read again.
constexpr int kEditDelayMs = 100;
}  // namespace

QIcon primvarSeverityIcon(PrimvarProblem::Severity severity) {
    static const std::vector<QIcon> icons = [] {
        std::vector<QIcon> made;
        for (const QColor& color : kSeverityColors) {
            QPixmap pixmap(20, 20);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(4, 4, 12, 12);
            painter.end();
            made.emplace_back(pixmap);
        }
        return made;
    }();
    return icons[size_t(severity)];
}

std::optional<PrimvarProblem::Severity> worstSeverity(const std::vector<PrimvarProblem>& problems) {
    std::optional<PrimvarProblem::Severity> worst;
    for (const PrimvarProblem& problem : problems) {
        if (!worst || problem.severity < *worst) worst = problem.severity;
    }
    return worst;
}

// --- PrimvarViewContext --------------------------------------------------------------

const PrimvarSet* PrimvarViewContext::setWith(const std::string& name) const {
    const std::string& wanted = name.empty() ? primvar : name;
    if (wanted.empty()) return nullptr;
    for (const PrimvarSet& set : sets) {
        if (set.find(wanted)) return &set;
    }
    return nullptr;
}

const PrimvarInfo* PrimvarViewContext::info(const std::string& name) const {
    const PrimvarSet* set = setWith(name);
    return set ? set->find(name.empty() ? primvar : name) : nullptr;
}

std::size_t PrimvarViewContext::primsWith(const std::string& name) const {
    const std::string& wanted = name.empty() ? primvar : name;
    return std::count_if(sets.begin(), sets.end(), [&wanted](const PrimvarSet& set) { return set.find(wanted) != nullptr; });
}

// --- PrimvarContextTracker -----------------------------------------------------------

PrimvarContextTracker::PrimvarContextTracker(StageManager* stageManager, QObject* parent)
    : QObject(parent), m_stageManager(stageManager), m_timer(new QTimer(this)) {
    m_timer->setSingleShot(true);
    m_timer->setInterval(kEditDelayMs);
    connect(m_timer, &QTimer::timeout, this, &PrimvarContextTracker::refresh);
    m_selectionConnection = stageManager->selectionChanged.connect([this](StageRefPtr stage, std::vector<SdfPath>) {
        if (stage == m_context.stage) refresh();
    });
}

PrimvarContextTracker::~PrimvarContextTracker() = default;

void PrimvarContextTracker::setStage(const StageRefPtr& stage) {
    m_changedConnection = {};
    m_timeConnection = {};
    m_focusConnection = {};
    m_context.stage = stage;
    if (stage) {
        // An edit can send several notices, and a drag many.
        m_changedConnection = stage->changed.connect([this](const StageChanges&) { m_timer->start(); });
        m_timeConnection = stage->timeChanged.connect([this](double) { m_timer->start(); });
        m_focusConnection = stage->primvarFocusChanged.connect([this](const std::string& name) {
            m_context.primvar = name;
            Q_EMIT contextChanged(true);
        });
    }
    refresh();
}

void PrimvarContextTracker::refresh() {
    m_timer->stop();
    const StageRefPtr& stage = m_context.stage;
    m_context.prims.clear();
    m_context.sets.clear();
    m_context.selected = 0;
    m_context.primvar.clear();
    m_context.time = UsdTimeCode::Default();
    if (stage) {
        const std::vector<SdfPath> selected = m_stageManager->selectedPaths(stage);
        m_context.selected = selected.size();
        m_context.primvar = stage->primvarFocus();
        m_context.time = stage->timeCode();
        for (const SdfPath& path : selected) {
            if (m_context.prims.size() == PrimvarViewContext::kMaxPrims) break;
            const UsdPrim prim = stage->usdStage()->GetPrimAtPath(path);
            if (!prim) continue;
            m_context.prims.push_back(path);
            m_context.sets.push_back(primvarInfo(prim, m_context.time));
        }
    }
    Q_EMIT contextChanged(false);
}

// --- PrimvarViews --------------------------------------------------------------------

PrimvarViews& PrimvarViews::instance() {
    static PrimvarViews* const views = new PrimvarViews;  // outlives everything that shows one
    return *views;
}

PrimvarViews::PrimvarViews() {
    m_types.push_back({QStringLiteral("info"), tr("Info"), [](QWidget* parent) { return new PrimvarInfoView(parent); }, {}});
}

void PrimvarViews::add(PrimvarViewType type) {
    const auto it = std::find_if(m_types.begin(), m_types.end(), [&type](const PrimvarViewType& other) { return other.id == type.id; });
    if (it != m_types.end()) {
        *it = std::move(type);
    } else {
        m_types.push_back(std::move(type));
    }
    Q_EMIT changed();
}

bool PrimvarViews::remove(const QString& id) {
    if (std::erase_if(m_types, [&id](const PrimvarViewType& type) { return type.id == id; }) == 0) return false;
    Q_EMIT changed();
    return true;
}

const PrimvarViewType* PrimvarViews::find(const QString& id) const {
    const auto it = std::find_if(m_types.begin(), m_types.end(), [&id](const PrimvarViewType& type) { return type.id == id; });
    return it == m_types.end() ? nullptr : &*it;
}

// --- PrimvarInfoView -----------------------------------------------------------------

class PrimvarInfoView::Browser : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;
};

PrimvarInfoView::PrimvarInfoView(QWidget* parent)
    : PrimvarView(parent), m_browser(new Browser(this)), m_generation(std::make_shared<int>(0)) {
    m_browser->setObjectName(QStringLiteral("primvarInfo"));
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_browser);
    render();
}

PrimvarInfoView::~PrimvarInfoView() = default;

QString PrimvarInfoView::text() const { return m_browser->toPlainText(); }

void PrimvarInfoView::setContext(const PrimvarViewContext& context, bool focusOnly) {
    m_context = context;
    const PrimvarSet* set = m_context.setWith();
    const std::string key = set ? set->prim.GetString() + '\n' + m_context.primvar : std::string();
    // An edit may have changed the values; another focus or prim certainly did.
    if (!focusOnly || key != m_statsKey) startStatistics();
    render();
}

void PrimvarInfoView::startStatistics() {
    ++*m_generation;
    m_hasStats = false;
    m_computing = false;
    m_statsKey.clear();
    const PrimvarSet* set = m_context.setWith();
    const PrimvarInfo* info = m_context.info();
    if (!set || !info || !info->hasValue || !m_context.stage) return;
    m_statsKey = set->prim.GetString() + '\n' + m_context.primvar;
    // Read here: the stage is the main thread's. A Vt array is shared, not copied.
    const UsdPrim source = m_context.stage->usdStage()->GetPrimAtPath(info->sourcePrim);
    const UsdAttribute values = source ? source.GetAttribute(TfToken("primvars:" + info->name)) : UsdAttribute();
    VtValue value;
    if (!values || !values.Get(&value, m_context.time)) return;
    VtIntArray indices;
    if (info->indexed) source.GetAttribute(TfToken("primvars:" + info->name + ":indices")).Get(&indices, m_context.time);
    m_computing = true;
    const int generation = *m_generation;
    const std::shared_ptr<int> current = m_generation;
    const QPointer<PrimvarInfoView> self(this);
    const int elementSize = info->elementSize;
    QThreadPool::globalInstance()->start([self, current, generation, value, indices, elementSize]() mutable {
        if (*current != generation) return;  // asked again meanwhile
        const bool flattened = flattenPrimvar(value, indices, elementSize);
        PrimvarStatistics stats = primvarStatistics(value);
        QMetaObject::invokeMethod(
            self.data(),
            [self, current, generation, flattened, stats = std::move(stats)]() mutable {
                if (!self || *current != generation) return;
                self->m_stats = std::move(stats);
                self->m_flattened = flattened;
                self->m_hasStats = true;
                self->m_computing = false;
                self->render();
                Q_EMIT self->statisticsReady();
            },
            Qt::QueuedConnection);
    });
}

void PrimvarInfoView::render() {
    QString out;
    const auto para = [&out](const QString& text) { out += QStringLiteral("<p>%1</p>").arg(text); };
    const auto row = [](const QString& label, const QString& value) {
        return QStringLiteral("<tr><td style='padding-right:12px;color:#8e8e93'>%1</td><td>%2</td></tr>").arg(label, value);
    };
    if (m_context.prims.empty()) {
        para(tr("Select a prim to see its primvars."));
        m_browser->setHtml(out);
        return;
    }
    const std::string& name = m_context.primvar;
    const PrimvarSet* set = m_context.setWith();
    const PrimvarInfo* info = m_context.info();
    if (name.empty()) {
        para(tr("Pick a primvar in the list."));
        m_browser->setHtml(out);
        return;
    }
    out += QStringLiteral("<h3>%1</h3>").arg(html(name));
    if (!info) {
        QStringList readers;
        for (const PrimvarSet& each : m_context.sets) {
            for (const MissingPrimvar& missing : each.missing) {
                if (missing.name != name) continue;
                for (const SdfPath& material : missing.materials) readers << html(material.GetString());
            }
        }
        readers.removeDuplicates();
        para(readers.isEmpty() ? tr("None of the selected prims has it.")
                               : tr("Read by %1, but none of the selected prims has it.").arg(readers.join(QStringLiteral(", "))));
        m_browser->setHtml(out);
        return;
    }

    if (m_context.prims.size() > 1) {
        para(tr("On %1 of %2 selected prims. Shown for %3.")
                 .arg(m_context.primsWith())
                 .arg(m_context.selected)
                 .arg(html(set->prim.GetString())));
    }
    QString table = QStringLiteral("<table>");
    table += row(tr("Type"), html(info->type.GetAsToken().GetString()));
    table += row(tr("Interpolation"), info->interpolationAuthored
                                          ? tr("%1, in %2").arg(html(info->interpolation.GetString()), layerName(info->interpolationLayer))
                                          : tr("%1 (not authored)").arg(html(info->interpolation.GetString())));
    if (info->elementSize != 1) table += row(tr("Element size"), QString::number(info->elementSize));
    QString source;
    switch (info->source) {
        case PrimvarSource::Local: source = tr("Authored here"); break;
        case PrimvarSource::Inherited: source = tr("Inherited from %1").arg(html(info->sourcePrim.GetString())); break;
        case PrimvarSource::Material: source = tr("From the material %1").arg(html(info->sourcePrim.GetString())); break;
    }
    table += row(tr("Source"), source);
    if (!info->hasValue) {
        table += row(tr("Values"), tr("None: declared only, or blocked"));
    } else {
        table += row(tr("Values"), tr("%1, in %2").arg(number(info->valueCount), layerName(info->valuesLayer)));
        table += row(tr("Indices"), info->indexed ? tr("%1, in %2").arg(number(info->elementCount), layerName(info->indicesLayer))
                                                  : tr("None"));
        if (info->indexed && info->unauthoredValuesIndex >= 0) {
            table += row(tr("No value at"), tr("index %1").arg(info->unauthoredValuesIndex));
        }
        table += row(tr("Elements"), info->expectedCount ? tr("%1 of %2 (%3)")
                                                              .arg(number(info->elementCount), number(*info->expectedCount),
                                                                   html(info->expectedOf))
                                                        : number(info->elementCount));
    }
    if (info->timeVarying) table += row(tr("Time"), tr("Varies; shown at the stage's time"));
    if (!info->idTarget.IsEmpty()) table += row(tr("Path of"), html(info->idTarget.GetString()));
    if (!info->readers.empty()) {
        QStringList readers;
        for (const SdfPath& material : info->readers) readers << html(material.GetString());
        table += row(tr("Read by"), readers.join(QStringLiteral("<br>")));
    }
    if (!info->derivedFrom.empty()) {
        QStringList from;
        for (const std::string& each : info->derivedFrom) from << html(each);
        table += row(tr("Derived from"), from.join(QStringLiteral(", ")));
    }
    for (const PrimvarGroup& group : set->groups) {
        if (std::find(group.members.begin(), group.members.end(), name) == group.members.end()) continue;
        QStringList members;
        for (const std::string& member : group.members) members << html(member);
        QString text = members.join(QStringLiteral(", "));
        if (!group.missing.empty()) {
            QStringList missing;
            for (const std::string& each : group.missing) missing << html(each);
            text += tr("; missing %1").arg(missing.join(QStringLiteral(", ")));
        }
        table += row(html(group.label), text);
    }
    table += QStringLiteral("</table>");
    out += table;

    std::vector<PrimvarProblem> problems = info->problems;
    for (const PrimvarGroup& group : set->groups) {
        if (std::find(group.members.begin(), group.members.end(), name) == group.members.end()) continue;
        problems.insert(problems.end(), group.problems.begin(), group.problems.end());
    }
    if (!problems.empty()) {
        out += QStringLiteral("<h4>%1</h4>").arg(tr("Problems"));
        for (const PrimvarProblem& problem : problems) {
            const QColor color = kSeverityColors[size_t(problem.severity)];
            para(QStringLiteral("<span style='color:%1'>%2</span>: %3")
                     .arg(color.name(), severityWord(problem.severity), html(problem.message)));
        }
    }

    out += QStringLiteral("<h4>%1</h4>").arg(tr("Statistics"));
    if (!info->hasValue) {
        para(tr("No values."));
    } else if (m_computing && !m_hasStats) {
        para(tr("Working them out..."));
    } else if (m_hasStats) {
        const PrimvarStatistics& stats = m_stats;
        QString table = QStringLiteral("<table>");
        table += row(m_flattened ? tr("Elements") : tr("Values"), number(stats.count));
        if (!m_flattened) table += row(QString(), tr("Of the values, as some indices are outside them"));
        if (!stats.min.empty()) {
            const auto list = [](const std::vector<double>& values) {
                QStringList parts;
                for (const double value : values) parts << number(value);
                return parts.size() == 1 ? parts.front() : QStringLiteral("(%1)").arg(parts.join(QStringLiteral(", ")));
            };
            table += row(tr("Min"), list(stats.min));
            table += row(tr("Max"), list(stats.max));
            table += row(tr("Mean"), list(stats.mean));
        }
        if (stats.nonFinite > 0) table += row(tr("Not finite"), number(stats.nonFinite));
        if (stats.distinct) {
            table += row(tr("Distinct"), number(*stats.distinct));
            QStringList commonest;
            for (const auto& [value, count] : stats.commonest) commonest << tr("%1 (%2)").arg(html(value), number(count));
            if (!commonest.isEmpty()) table += row(tr("Commonest"), commonest.join(QStringLiteral(", ")));
        }
        table += QStringLiteral("</table>");
        out += table;
    }
    m_browser->setHtml(out);
}

}  // namespace usdcc::usd
