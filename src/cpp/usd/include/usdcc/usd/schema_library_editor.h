#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/schema_generator.h"
#include "usdcc/usd/schema_libraries.h"
#include "usdcc/usd/schema_library.h"

#include <QString>
#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class QToolButton;

namespace usdcc::usd {

class SchemaExplorerViewPanel;

// What the app gives the Schema Editor (docs/SCHEMAS.md §5): its Python, to run
// usdGenSchema with; a restart, as new schemas load only at a start; a text
// editor for Edit as Text; and the Schemas panel, for Promote to Schema.
struct SchemaEditorHooks {
    SchemaPython python;
    // Closes the app, asking what closing asks, and starts it again with the
    // same session. False if it didn't (the user cancelled).
    std::function<bool()> restart;
    // Opens a file to edit as text; the form reads it again when it changes.
    std::function<void(const QString& path)> editText;
    // A Schemas panel, raised, opening one if there's none.
    std::function<SchemaExplorerViewPanel*()> showSchemasPanel;
};
USDCC_USD_UI_API void setSchemaEditorHooks(SchemaEditorHooks hooks);
USDCC_USD_UI_API const SchemaEditorHooks& schemaEditorHooks();

// The Schema Editor (docs/SCHEMAS.md §5.2): a form over one codeless library's
// schema.usda, in the Schemas panel. Classes on the left; the chosen one's
// name, kind, base, documentation, built-in APIs, what an API applies itself
// to or can apply to, a multiple-apply API's prefix, and its properties in a
// table. The checks of §5.4 run as you type. Save writes schema.usda through
// Sdf (writeSchemaLibrary()); Generate saves, says what it would change for
// files that use the library now (§5.5), and generates it off the UI thread;
// the result loads at the next start, which Restart does.
class USDCC_USD_UI_API SchemaLibraryEditor : public QWidget {
    Q_OBJECT

public:
    explicit SchemaLibraryEditor(QWidget* parent = nullptr);
    ~SchemaLibraryEditor() override;

    // Reads the library's schema.usda. False, with `error`, if it can't.
    bool open(const QString& library, QString* error = nullptr);
    QString library() const;
    const SchemaLibraryDef& definition() const { return m_def; }
    bool isModified() const { return m_modified; }
    bool save(QString* error = nullptr);
    // Reads schema.usda again, dropping what isn't saved.
    bool reload(QString* error = nullptr);
    // Asks to save unsaved edits: false if the user cancelled.
    bool maybeSave();

    bool selectClass(const QString& name);
    QString currentClass() const;
    // A new class of `kind`, named for it and made unique, chosen.
    void addClass(SchemaInfo::Kind kind);
    void removeCurrentClass();
    // A new property on the chosen class, named to be unique; its row.
    int addProperty(bool relationship);
    void removeProperty(int row);

    // The checks of the definition as it is now.
    std::vector<SchemaIssue> issues() const;

    // Saves and generates, off the UI thread; `ask` first asks when it would
    // change files that use the library now. generated() says how it went.
    void generate(bool ask = true);
    bool isGenerating() const { return m_generating; }
    const SchemaGenerateResult& lastResult() const { return m_lastResult; }

    // For tests and scripts.
    QListWidget* classList() const { return m_classes; }
    QTableWidget* propertyTable() const { return m_properties; }
    QLineEdit* nameEdit() const { return m_name; }
    QComboBox* kindCombo() const { return m_kind; }
    QComboBox* baseCombo() const { return m_base; }
    QPlainTextEdit* documentationEdit() const { return m_documentation; }
    QLineEdit* builtInEdit() const { return m_builtIn; }
    QLineEdit* autoApplyEdit() const { return m_autoApply; }
    QLineEdit* canApplyEdit() const { return m_canApply; }
    QLineEdit* prefixEdit() const { return m_prefix; }
    QListWidget* issueList() const { return m_issues; }
    QLabel* statusLabel() const { return m_status; }
    QPushButton* restartButton() const { return m_restart; }

Q_SIGNALS:
    void closeRequested();
    // Saved or generated: what the Schemas panel shows of the library changed.
    void libraryChanged();
    void generated(bool ok);

private:
    void buildClassForm();
    void showClass();
    void showProperties();
    void refreshClassList();
    void refreshIssues();
    void refreshStatus();
    void classEdited();
    void propertyEdited(int row);
    void markModified();
    SchemaClassDef* current();
    void finishGenerate(const SchemaGenerateResult& result);

    QString m_library;
    SchemaLibraryDef m_def;
    bool m_modified = false;
    bool m_updating = false;  // filling the form: its signals aren't edits
    bool m_generating = false;
    // schema.usda's time and size as the form last read or wrote it: a change
    // that isn't the form's own is someone else's (Edit as Text, Promote).
    QString m_known;
    SchemaGenerateResult m_lastResult;
    int m_current = -1;

    QLabel* m_title;
    QLabel* m_status;
    QPushButton* m_save;
    QPushButton* m_generate;
    QPushButton* m_restart;
    QListWidget* m_classes;
    QToolButton* m_addClass;
    QToolButton* m_removeClass;
    QWidget* m_form;
    QLineEdit* m_name;
    QComboBox* m_kind;
    QComboBox* m_base;
    QLabel* m_baseLabel;
    QPlainTextEdit* m_documentation;
    QLineEdit* m_builtIn;
    QLineEdit* m_autoApply;
    QLabel* m_autoApplyLabel;
    QLineEdit* m_canApply;
    QLabel* m_canApplyLabel;
    QLineEdit* m_prefix;
    QLabel* m_prefixLabel;
    QTableWidget* m_properties;
    QListWidget* m_issues;
    QTimer* m_checkTimer;
};

}  // namespace usdcc::usd
