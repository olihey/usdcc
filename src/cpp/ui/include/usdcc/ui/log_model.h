#pragma once

#include "usdcc/core/log.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QRegularExpression>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTimer>

#include <array>
#include <deque>
#include <memory>

namespace usdcc::ui {

// The log as a table (docs/LOGGING.md §4): one row per LogRecord a LogHub
// delivers, starting with the hub's backlog. Records arrive on any thread;
// the model only queues them there and takes them in on the UI thread, at
// most every 50 ms and all at once, so a burst of thousands of messages is a
// handful of row insertions rather than thousands of signals.
class LogModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { TimeColumn, LevelColumn, SourceColumn, CategoryColumn, MessageColumn, ColumnCount };
    enum Role {
        LevelRole = Qt::UserRole + 1,  // int(LogLevel)
        SourceRole,                    // QString
        CategoryRole,                  // QString
        SequenceRole,                  // qulonglong
    };

    // Subscribes to `hub` (with its backlog) until destroyed.
    explicit LogModel(usdcc::core::LogHub& hub = usdcc::core::LogHub::instance(), QObject* parent = nullptr);
    ~LogModel() override;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    const usdcc::core::LogRecord& record(int row) const { return m_records[static_cast<std::size_t>(row)]; }

    // How many rows of each level the model holds.
    int count(usdcc::core::LogLevel level) const { return m_counts[static_cast<std::size_t>(level)]; }
    // Every source seen since the model was created, in first-seen order.
    const QStringList& sources() const { return m_sources; }
    // Every category seen since the model was created, in first-seen order.
    const QStringList& categories() const { return m_categories; }

    // Oldest rows are dropped past this many (the hub's capacity by default).
    void setCapacity(int capacity);
    // Removes every row. The hub and the log file keep them.
    void clear();
    // Takes in whatever has arrived without waiting for the next batch.
    void drainNow();

    // A record's details as rich text (the tooltip of every cell in its row):
    // full message and details block, then time, level, source, category,
    // location, function, thread and repeats.
    static QString detailsHtml(const usdcc::core::LogRecord& record);

    // The color a level is shown in (from the dark theme's tokens).
    static QColor levelColor(usdcc::core::LogLevel level);
    static QString levelName(usdcc::core::LogLevel level);

Q_SIGNALS:
    // After each batch of new rows (`errors` of them Error or Fatal).
    void recordsAdded(int count, int errors);
    void countsChanged();

private:
    struct Inbox;
    class Sink;

    void scheduleDrain();
    void drain();
    int rowForSequence(std::uint64_t sequence) const;
    void countRow(const usdcc::core::LogRecord& record, int delta);

    usdcc::core::LogHub& m_hub;
    std::shared_ptr<Inbox> m_inbox;
    std::shared_ptr<Sink> m_sink;
    QTimer m_drainTimer;
    std::deque<usdcc::core::LogRecord> m_records;
    int m_capacity;
    std::array<int, 6> m_counts{};
    QStringList m_sources;
    QStringList m_categories;
    QSet<QString> m_categorySet;  // m_categories, for lookups
};

// What the Log panel shows of a LogModel: the enabled levels, minus hidden
// sources and categories, matching the search text (in message, category,
// source or details; plain or regular expression, either case).
//
// Categories form a tree, '.' separating its levels: hiding (or showing only)
// "usdcc.viewport" covers "usdcc.viewport" itself and "usdcc.viewport.picking",
// but not "usdcc.viewportx".
class LogFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit LogFilterModel(QObject* parent = nullptr);

    // `category` is `node` or lies under it in the category tree.
    static bool coversCategory(const QString& node, const QString& category);

    void setLevelShown(usdcc::core::LogLevel level, bool shown);
    bool isLevelShown(usdcc::core::LogLevel level) const;
    void setSourceHidden(const QString& source, bool hidden);
    bool isSourceHidden(const QString& source) const;
    const QSet<QString>& hiddenSources() const { return m_hiddenSources; }
    // `category` is a node of the category tree (see above).
    void setCategoryHidden(const QString& category, bool hidden);
    // Replaces every hidden node at once.
    void setHiddenCategories(const QSet<QString>& categories);
    const QSet<QString>& hiddenCategories() const { return m_hiddenCategories; }
    // Whether `category` is a hidden node or lies under one.
    bool isCategoryHidden(const QString& category) const;
    void showAllCategories();
    // Shows only this node of the category tree and what lies under it; empty
    // shows every category.
    void setOnlyCategory(const QString& category);
    const QString& onlyCategory() const { return m_onlyCategory; }
    void setSearch(const QString& text, bool caseSensitive, bool regularExpression);
    // False when the search is a regular expression that doesn't parse.
    bool isSearchValid() const { return m_searchValid; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    int m_levelMask;
    QSet<QString> m_hiddenSources;
    QSet<QString> m_hiddenCategories;
    QString m_onlyCategory;
    QString m_searchText;
    bool m_caseSensitive = false;
    bool m_regularExpression = false;
    bool m_searchValid = true;
    QRegularExpression m_searchExpression;
};

}  // namespace usdcc::ui
