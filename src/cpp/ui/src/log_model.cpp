#include "usdcc/ui/log_model.h"

#include <QDateTime>

#include <algorithm>
#include <mutex>
#include <vector>

namespace usdcc::ui {

using usdcc::core::LogLevel;
using usdcc::core::LogRecord;

namespace {
// How long records wait before the UI thread takes them in, so a burst
// becomes one batch.
constexpr int kDrainIntervalMs = 50;

// Fatal/Error and Trace/Debug share a filter toggle in the panel, but the
// filter itself is per level.
int levelBit(LogLevel level) { return 1 << static_cast<int>(level); }
constexpr int kDefaultLevelMask = (1 << static_cast<int>(LogLevel::Info)) | (1 << static_cast<int>(LogLevel::Warning)) |
                                  (1 << static_cast<int>(LogLevel::Error)) | (1 << static_cast<int>(LogLevel::Fatal));

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString formatTime(std::chrono::system_clock::time_point time) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(ms.count()).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

QString firstLine(const std::string& text) {
    const std::size_t newline = text.find('\n');
    return QString::fromStdString(newline == std::string::npos ? text : text.substr(0, newline));
}
}  // namespace

// Where the sink leaves records for the UI thread.
struct LogModel::Inbox {
    std::mutex mutex;
    std::vector<LogRecord> added;
    std::vector<LogRecord> repeated;
    bool scheduled = false;  // a drain is already on its way to the UI thread
};

// Runs on whichever thread posted, under the hub's lock: only queues.
class LogModel::Sink : public usdcc::core::LogSink {
public:
    Sink(LogModel* model, std::shared_ptr<Inbox> inbox) : m_model(model), m_inbox(std::move(inbox)) {}

    void onRecord(const LogRecord& record) override {
        std::lock_guard lock(m_inbox->mutex);
        m_inbox->added.push_back(record);
        wake();
    }
    void onRepeat(const LogRecord& record) override {
        std::lock_guard lock(m_inbox->mutex);
        m_inbox->repeated.push_back(record);
        wake();
    }

private:
    void wake() {
        if (m_inbox->scheduled) return;
        m_inbox->scheduled = true;
        // The model is alive here: its destructor removes this sink first, and
        // Qt drops the queued call if the model is gone by the time it runs.
        QMetaObject::invokeMethod(m_model, &LogModel::scheduleDrain, Qt::QueuedConnection);
    }

    LogModel* m_model;
    std::shared_ptr<Inbox> m_inbox;
};

LogModel::LogModel(usdcc::core::LogHub& hub, QObject* parent)
    : QAbstractTableModel(parent),
      m_hub(hub),
      m_inbox(std::make_shared<Inbox>()),
      m_capacity(static_cast<int>(hub.capacity())) {
    m_drainTimer.setSingleShot(true);
    m_drainTimer.setInterval(kDrainIntervalMs);
    connect(&m_drainTimer, &QTimer::timeout, this, &LogModel::drain);

    m_sink = std::make_shared<Sink>(this, m_inbox);
    m_hub.addSink(m_sink);
}

LogModel::~LogModel() { m_hub.removeSink(m_sink.get()); }

int LogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_records.size());
}

int LogModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant LogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const LogRecord& rec = record(index.row());

    switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
                case TimeColumn: {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(rec.time.time_since_epoch());
                    return QDateTime::fromMSecsSinceEpoch(ms.count()).toString(QStringLiteral("HH:mm:ss.zzz"));
                }
                case LevelColumn: return levelName(rec.level);
                case SourceColumn: return qs(rec.source);
                case CategoryColumn: return qs(rec.category);
                case MessageColumn: {
                    QString text = firstLine(rec.message);
                    if (rec.repeatCount > 1) text += QStringLiteral("  ×%1").arg(rec.repeatCount);
                    return text;
                }
            }
            return {};
        case Qt::ForegroundRole:
            if (index.column() == LevelColumn || rec.level >= LogLevel::Error || rec.level <= LogLevel::Debug) {
                return levelColor(rec.level);
            }
            return {};
        case Qt::ToolTipRole: return detailsHtml(rec);
        case LevelRole: return static_cast<int>(rec.level);
        case SourceRole: return qs(rec.source);
        case CategoryRole: return qs(rec.category);
        case SequenceRole: return static_cast<qulonglong>(rec.sequence);
    }
    return {};
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case TimeColumn: return tr("Time");
        case LevelColumn: return tr("Level");
        case SourceColumn: return tr("Source");
        case CategoryColumn: return tr("Category");
        case MessageColumn: return tr("Message");
    }
    return {};
}

void LogModel::setCapacity(int capacity) {
    m_capacity = std::max(capacity, 1);
    const int excess = rowCount() - m_capacity;
    if (excess > 0) {
        beginRemoveRows({}, 0, excess - 1);
        for (int i = 0; i < excess; ++i) {
            countRow(m_records.front(), -1);
            m_records.pop_front();
        }
        endRemoveRows();
        Q_EMIT countsChanged();
    }
}

void LogModel::clear() {
    drainNow();
    beginResetModel();
    m_records.clear();
    m_counts.fill(0);
    endResetModel();
    Q_EMIT countsChanged();
}

void LogModel::drainNow() {
    m_drainTimer.stop();
    drain();
}

QString LogModel::detailsHtml(const LogRecord& record) {
    // Very long messages (a dumped layer, say) are cut: a tooltip can't scroll.
    auto escaped = [](const std::string& text) {
        constexpr std::size_t kMaxLength = 2000;
        QString result = qs(text.size() > kMaxLength ? text.substr(0, kMaxLength) : text).toHtmlEscaped();
        if (text.size() > kMaxLength) result += QStringLiteral(" \u2026");
        return result;
    };

    QString html = QStringLiteral("<div style=\"white-space: pre-wrap;\">%1</div>").arg(escaped(record.message));
    if (!record.details.empty()) {
        html += QStringLiteral("<pre>%1</pre>").arg(escaped(record.details));
    }

    QString rows;
    auto row = [&rows](const QString& name, const QString& value) {
        rows += QStringLiteral("<tr><td style=\"color: rgba(255,255,255,40%); padding-right: 12px;\">%1</td>"
                               "<td>%2</td></tr>")
                    .arg(name, value);
    };
    row(tr("Time"), formatTime(record.time));
    row(tr("Level"), QStringLiteral("<span style=\"color: %1;\">%2</span>")
                         .arg(levelColor(record.level).name(), levelName(record.level)));
    row(tr("Source"), qs(record.source).toHtmlEscaped());
    row(tr("Category"), qs(record.category).toHtmlEscaped());
    if (!record.file.empty()) {
        // On one line: rich text would otherwise break a path after "C:".
        row(tr("Location"), QStringLiteral("<span style=\"white-space: nowrap;\">%1:%2</span>")
                                .arg(qs(record.file).toHtmlEscaped())
                                .arg(record.line));
    }
    if (!record.function.empty()) row(tr("Function"), qs(record.function).toHtmlEscaped());
    row(tr("Thread"), record.threadName.empty()
                          ? QString::number(record.threadId)
                          : QStringLiteral("%1 (%2)").arg(qs(record.threadName).toHtmlEscaped()).arg(record.threadId));
    if (record.repeatCount > 1) {
        row(tr("Repeated"), tr("%1 times, last at %2").arg(record.repeatCount).arg(formatTime(record.lastTime)));
    }
    return html + QStringLiteral("<table style=\"margin-top: 6px;\">%1</table>").arg(rows);
}

QColor LogModel::levelColor(LogLevel level) {
    // From the dark theme (resources/theme/dark.qss): the axis red for errors,
    // an amber in the same family for warnings, and the primary and muted
    // text tints for the rest.
    switch (level) {
        case LogLevel::Fatal:
        case LogLevel::Error: return QColor(0xff, 0x5f, 0x56);
        case LogLevel::Warning: return QColor(0xff, 0xbd, 0x2e);
        case LogLevel::Info: return QColor(255, 255, 255, 191);
        case LogLevel::Debug:
        case LogLevel::Trace: return QColor(255, 255, 255, 102);
    }
    return QColor(255, 255, 255, 191);
}

QString LogModel::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return tr("Trace");
        case LogLevel::Debug: return tr("Debug");
        case LogLevel::Info: return tr("Info");
        case LogLevel::Warning: return tr("Warning");
        case LogLevel::Error: return tr("Error");
        case LogLevel::Fatal: return tr("Fatal");
    }
    return {};
}

void LogModel::scheduleDrain() {
    if (!m_drainTimer.isActive()) m_drainTimer.start();
}

void LogModel::drain() {
    std::vector<LogRecord> added;
    std::vector<LogRecord> repeated;
    {
        std::lock_guard lock(m_inbox->mutex);
        added.swap(m_inbox->added);
        repeated.swap(m_inbox->repeated);
        m_inbox->scheduled = false;
    }
    if (added.empty() && repeated.empty()) return;

    int errors = 0;
    int incoming = 0;
    if (!added.empty()) {
        // More than fit: only the newest matter.
        auto first = added.begin();
        if (added.size() > static_cast<std::size_t>(m_capacity)) first = added.end() - m_capacity;
        incoming = static_cast<int>(added.end() - first);

        const int excess = std::min(rowCount() + incoming - m_capacity, rowCount());
        if (excess > 0) {
            beginRemoveRows({}, 0, excess - 1);
            for (int i = 0; i < excess; ++i) {
                countRow(m_records.front(), -1);
                m_records.pop_front();
            }
            endRemoveRows();
        }

        beginInsertRows({}, rowCount(), rowCount() + incoming - 1);
        for (auto it = first; it != added.end(); ++it) {
            countRow(*it, +1);
            if (it->level >= LogLevel::Error) ++errors;
            const QString source = qs(it->source);
            if (!m_sources.contains(source)) m_sources << source;
            const QString category = qs(it->category);
            if (!m_categorySet.contains(category)) {
                m_categorySet.insert(category);
                m_categories << category;
            }
            m_records.push_back(std::move(*it));
        }
        endInsertRows();
    }

    for (const LogRecord& repeat : repeated) {
        const int row = rowForSequence(repeat.sequence);
        if (row < 0) continue;
        LogRecord& existing = m_records[static_cast<std::size_t>(row)];
        existing.repeatCount = repeat.repeatCount;
        existing.lastTime = repeat.lastTime;
        Q_EMIT dataChanged(index(row, MessageColumn), index(row, MessageColumn));
    }

    if (incoming > 0) {
        Q_EMIT recordsAdded(incoming, errors);
        Q_EMIT countsChanged();
    }
}

int LogModel::rowForSequence(std::uint64_t sequence) const {
    auto it = std::lower_bound(m_records.begin(), m_records.end(), sequence,
                               [](const LogRecord& record, std::uint64_t value) { return record.sequence < value; });
    return it != m_records.end() && it->sequence == sequence ? static_cast<int>(it - m_records.begin()) : -1;
}

void LogModel::countRow(const LogRecord& record, int delta) { m_counts[static_cast<std::size_t>(record.level)] += delta; }

// ---------------------------------------------------------------------------

LogFilterModel::LogFilterModel(QObject* parent) : QSortFilterProxyModel(parent), m_levelMask(kDefaultLevelMask) {}

void LogFilterModel::setLevelShown(LogLevel level, bool shown) {
    const int mask = shown ? (m_levelMask | levelBit(level)) : (m_levelMask & ~levelBit(level));
    if (mask == m_levelMask) return;
    m_levelMask = mask;
    invalidateRowsFilter();
}

bool LogFilterModel::isLevelShown(LogLevel level) const { return (m_levelMask & levelBit(level)) != 0; }

void LogFilterModel::setSourceHidden(const QString& source, bool hidden) {
    if (hidden == m_hiddenSources.contains(source)) return;
    if (hidden) {
        m_hiddenSources.insert(source);
    } else {
        m_hiddenSources.remove(source);
    }
    invalidateRowsFilter();
}

bool LogFilterModel::isSourceHidden(const QString& source) const { return m_hiddenSources.contains(source); }

void LogFilterModel::setCategoryHidden(const QString& category, bool hidden) {
    if (hidden == m_hiddenCategories.contains(category)) return;
    if (hidden) {
        m_hiddenCategories.insert(category);
    } else {
        m_hiddenCategories.remove(category);
    }
    invalidateRowsFilter();
}

void LogFilterModel::setHiddenCategories(const QSet<QString>& categories) {
    if (categories == m_hiddenCategories) return;
    m_hiddenCategories = categories;
    invalidateRowsFilter();
}

bool LogFilterModel::coversCategory(const QString& node, const QString& category) {
    return category.startsWith(node) && (category.size() == node.size() || category.at(node.size()) == u'.');
}

bool LogFilterModel::isCategoryHidden(const QString& category) const {
    for (const QString& hidden : m_hiddenCategories) {
        if (coversCategory(hidden, category)) return true;
    }
    return false;
}

void LogFilterModel::showAllCategories() {
    if (m_hiddenCategories.isEmpty() && m_onlyCategory.isEmpty()) return;
    m_hiddenCategories.clear();
    m_onlyCategory.clear();
    invalidateRowsFilter();
}

void LogFilterModel::setOnlyCategory(const QString& category) {
    if (category == m_onlyCategory) return;
    m_onlyCategory = category;
    invalidateRowsFilter();
}

void LogFilterModel::setSearch(const QString& text, bool caseSensitive, bool regularExpression) {
    m_searchText = text;
    m_caseSensitive = caseSensitive;
    m_regularExpression = regularExpression;
    m_searchValid = true;
    if (regularExpression && !text.isEmpty()) {
        m_searchExpression.setPattern(text);
        m_searchExpression.setPatternOptions(caseSensitive ? QRegularExpression::NoPatternOption
                                                           : QRegularExpression::CaseInsensitiveOption);
        m_searchValid = m_searchExpression.isValid();
    }
    invalidateRowsFilter();
}

bool LogFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex&) const {
    const auto* model = static_cast<const LogModel*>(sourceModel());
    const LogRecord& record = model->record(sourceRow);
    if (!isLevelShown(record.level)) return false;

    const QString category = qs(record.category);
    if (!m_onlyCategory.isEmpty() && !coversCategory(m_onlyCategory, category)) return false;
    if (isCategoryHidden(category)) return false;
    const QString source = qs(record.source);
    if (m_hiddenSources.contains(source)) return false;

    // An invalid expression filters nothing out; the panel marks the field.
    if (m_searchText.isEmpty() || !m_searchValid) return true;
    const QString fields[] = {qs(record.message), category, source, qs(record.details)};
    for (const QString& field : fields) {
        const bool match = m_regularExpression
                               ? m_searchExpression.match(field).hasMatch()
                               : field.contains(m_searchText, m_caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        if (match) return true;
    }
    return false;
}

}  // namespace usdcc::ui
