#pragma once

#include "usdcc/usd/stage.h"

#include <QHash>
#include <QObject>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QUndoStack;

namespace usdcc::usd {

class UndoManager final : public QObject {
    Q_OBJECT
public:
    explicit UndoManager(QObject* parent = nullptr);
    QUndoStack* stack(const StageRefPtr& stage);
    bool apply(const StageRefPtr& stage, const QString& description, const std::function<bool()>& edit);

private:
    struct Entry {
        StageRefPtr stage;
        std::unique_ptr<QUndoStack> stack;
    };
    QHash<qulonglong, Entry*> m_entries;
    std::vector<std::unique_ptr<Entry>> m_storage;
};

}  // namespace usdcc::usd
