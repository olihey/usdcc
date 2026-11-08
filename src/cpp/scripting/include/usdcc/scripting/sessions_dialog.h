#pragma once

#include <QDialog>
#include <QImage>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

namespace usdcc::scripting {

class SessionController;

// File > Sessions... (docs/SESSIONS.md §6): the saved sessions with a preview each,
// newest first, and Load, Save, Rename, Delete, Import, Export and Open Folder.
class SessionsDialog : public QDialog {
    Q_OBJECT

public:
    // `preview`: the window as it was before the dialog opened, for Save.
    SessionsDialog(SessionController* sessions, const QImage& preview, QWidget* parent = nullptr);
    ~SessionsDialog() override;

    // File > Sessions...: takes the preview first, then shows the dialog.
    static void showDialog(SessionController* sessions, QWidget* parent);

    QListWidget* list() const { return m_list; }
    QLineEdit* nameField() const { return m_name; }
    // The selected session's name, or empty.
    QString selected() const;
    void select(const QString& name);

    bool loadSelected();
    bool saveAs(const QString& name);
    bool renameSelected(const QString& to);
    bool deleteSelected();
    bool importFile(const QString& path);
    bool exportSelected(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    void updateDetails();
    void loadNextThumbnail();
    // The selected session's preview, at the size its place has now.
    void showPreview();

    SessionController* m_sessions;
    QImage m_preview;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_image = nullptr;
    QLabel* m_details = nullptr;
    QLineEdit* m_name = nullptr;
    QPushButton* m_load = nullptr;
    QPushButton* m_rename = nullptr;
    QPushButton* m_delete = nullptr;
    QPushButton* m_export = nullptr;
    QTimer* m_thumbnails = nullptr;
    QImage m_shown;  // the selected session's full preview
};

}  // namespace usdcc::scripting
