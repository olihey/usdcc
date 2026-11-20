#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/schema_library.h"

#include <pxr/usd/usd/prim.h>

#include <QDialog>
#include <QStringList>

#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;

namespace usdcc::usd {

// Make Schema from Attributes (docs/SCHEMAS.md §6): the prim's properties no
// schema defines, each with a checkbox, its type and its value, which becomes
// the fallback; the library to add the API to (or a new one); and the API's
// name. The names stay as they are, so the prims that have them can be given
// the API once it's generated and loaded.
class USDCC_USD_UI_API PromoteSchemaDialog : public QDialog {
    Q_OBJECT

public:
    // `checked`: the properties ticked to start with; all, if empty.
    PromoteSchemaDialog(const PXR_NS::UsdPrim& prim, const QStringList& checked, QWidget* parent = nullptr);

    std::vector<SchemaPropertyDef> chosen() const;
    QString library() const;
    QString apiName() const;
    // Ticks exactly `names`.
    void setChosen(const QStringList& names);
    void setLibrary(const QString& library);
    void setApiName(const QString& name);

    QTreeWidget* propertyList() const { return m_list; }
    QComboBox* libraryCombo() const { return m_library; }
    QLineEdit* nameEdit() const { return m_name; }

    // Adds the API to the library (a new one first, if that's what's chosen).
    // False, with `error`, if that failed: the dialog stays open.
    bool apply(QString* error);

public Q_SLOTS:
    void accept() override;

private:
    void suggestName();

    std::vector<SchemaPropertyDef> m_properties;
    QTreeWidget* m_list;
    QComboBox* m_library;
    QLineEdit* m_name;
    QLabel* m_note;
    bool m_nameEdited = false;
};

// The dialog, then the Schemas panel's editor at the new API. `checked` as for
// the dialog. False if cancelled or there's nothing to promote.
USDCC_USD_UI_API bool promoteToSchemaInteractively(QWidget* parent, const PXR_NS::UsdPrim& prim, const QStringList& checked);

}  // namespace usdcc::usd
