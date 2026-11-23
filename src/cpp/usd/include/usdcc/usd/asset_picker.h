#pragma once

// The asset picker (docs/ASSET_PICKER.md): one dialog for choosing a file on disk or on a
// server that can be browsed, in place of the system file dialog where usdcc asks for a USD
// file or an asset path. Places (favourites, this PC, recent folders, each protocol's
// roots), a scheme dropdown with the path, and the folder's entries, listed on a worker
// thread.

#include "usdcc/core/export.h"

#include <pxr/pxr.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/sdf/layer.h>

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace usdcc::assets {
class Cancel;
}

namespace usdcc::usd {

struct AssetPickerOptions {
    QString title;
    // "USD Files (*.usd *.usda *.usdc *.usdz)", ...; All Files is added when missing.
    QStringList filters;
    // A folder or file, on disk or on a server, to start in. Empty: the last folder used.
    QString start;
    // The stage's, so a server's path means its environment's servers.
    PXR_NS::ArResolverContext context;
    // When the result goes into a layer: offers Relative and Absolute, relative to it.
    PXR_NS::SdfLayerHandle anchorLayer;
    // A file to save to rather than one to open (docs/ASSET_PICKER.md §5): a Save button, New
    // Folder, a question before replacing a file, and only places that can be written. On a
    // server the file goes to the first server of the stack that isn't read-only, which the
    // dialog names. A name without an extension gets the filter's first.
    bool save = false;
};

// ---- Favourites (§3.1): folders, the user's own, in usdcc.ini ------------------------------

struct AssetFavorite {
    QString label;
    QString path;  // a folder: an absolute path on disk, or a server's logical path ending in '/'
};

USDCC_USD_UI_API std::vector<AssetFavorite> assetFavorites();
// False when it's a favourite already. `label` empty: the folder's name, with its parent's
// when another favourite has that name.
USDCC_USD_UI_API bool addAssetFavorite(const QString& folder, const QString& label = {});
USDCC_USD_UI_API bool removeAssetFavorite(const QString& folder);
USDCC_USD_UI_API bool renameAssetFavorite(const QString& folder, const QString& label);
// Moves it `delta` places (-1 up, 1 down) in the list.
USDCC_USD_UI_API bool moveAssetFavorite(const QString& folder, int delta);
USDCC_USD_UI_API bool isAssetFavorite(const QString& folder);
// One spelling of a folder: on disk absolute with forward slashes; on a server its logical
// path (no stack id) ending in '/'.
USDCC_USD_UI_API QString normalizedAssetFolder(const QString& folder);

// The folders files were last picked from, newest first.
USDCC_USD_UI_API QStringList recentAssetFolders();

// The dropdown's schemes, in order: "file", the ones a source can browse, then the rest
// OpenUSD has a resolver for (§3.2).
USDCC_USD_UI_API QStringList assetPickerSchemes();
// Whether pickAsset() shows the picker: some source can browse. Otherwise the system dialog.
USDCC_USD_UI_API bool assetPickerAvailable();

// The file chosen, as it should be written: with an anchor layer, relative to it when
// the user asked for that and it's possible; else a file's absolute path or a server's
// logical path. Empty when cancelled.
USDCC_USD_UI_API QString pickAsset(QWidget* parent, const AssetPickerOptions& options);
// pickAsset() for a file to save to (AssetPickerOptions::save): a path on disk or a server's
// logical path, asked about first when it exists. The system's save dialog when nothing can
// be browsed. Empty when cancelled.
USDCC_USD_UI_API QString pickSaveAsset(QWidget* parent, AssetPickerOptions options);
// `chosen` (a file's path or a server's) as written into `anchor`: relative when asked and
// possible (§4).
USDCC_USD_UI_API QString assetPathForChoice(const QString& chosen, const PXR_NS::SdfLayerHandle& anchor, bool relative);

class USDCC_USD_UI_API AssetPickerDialog : public QDialog {
    Q_OBJECT

public:
    explicit AssetPickerDialog(const AssetPickerOptions& options, QWidget* parent = nullptr);
    ~AssetPickerDialog() override;

    // The file chosen: a path on disk or a server's logical path. Empty until accepted.
    QString chosenFile() const { return m_chosen; }
    // As it should be written (see pickAsset()).
    QString result() const;

    // The folder shown: a path on disk, "" for this PC's drives, a server's folder
    // ("webdav://studio/chars/"), or a scheme's roots ("webdav://").
    QString location() const { return m_location; }
    void navigate(const QString& location);
    // While a folder is being listed.
    bool listing() const { return m_listing; }

    void accept() override;

    // For tests.
    QComboBox* schemeBox() const { return m_scheme; }
    QLineEdit* pathEdit() const { return m_path; }
    QToolButton* favoriteButton() const { return m_favorite; }
    QTreeWidget* placesView() const { return m_places; }
    QTreeWidget* entriesView() const { return m_entries; }
    QLineEdit* fileNameEdit() const { return m_fileName; }
    QComboBox* filterBox() const { return m_filter; }
    QRadioButton* relativeButton() const { return m_relative; }
    QLabel* statusLabel() const { return m_status; }
    // Save mode: where a file saved in the folder shown goes, or why it can't.
    QString saveNote() const;
    // Save mode: a folder named `name` in the one shown, which is then shown. On a server it's
    // made when a file is saved in it, as a server makes the folders a file is written in.
    bool newFolder(const QString& name);
    // Save mode: what asks before a file is replaced; a message box unless set (tests).
    void setReplaceQuestion(std::function<bool(const QString& file)> ask) { m_askReplace = std::move(ask); }

private:
    struct Listed;
    void buildPlaces();
    // With `loginCredential`, the error is a refusal a login may cure: a Log In button
    // under it opens the login dialog for that credential, and lists again after.
    void showEntries(const std::vector<Listed>& entries, const QString& error, const QString& loginCredential = {},
                     const QString& loginScheme = {});
    // A page of the folder, from `from` (empty: its start, in place of what's shown; else
    // added to it). While it has more, a Show More row under it asks for the next.
    void listOnWorker(const QString& location, const std::string& from = {});
    void pathEntered();
    void schemeChosen(int index);
    void goUp();
    void toggleFavorite();
    void updateFavoriteButton();
    void placesMenu(const QPoint& at);
    void entriesMenu(const QPoint& at);
    void systemDialog();
    bool acceptCandidate(const QString& candidate, bool listed);
    bool acceptSave(const QString& candidate);
    void updateSaveNote();
    QString withDefaultSuffix(const QString& file) const;
    QString childOf(const QString& name) const;
    QStringList patterns() const;
    void setStatus(const QString& text);

    AssetPickerOptions m_options;
    QString m_location;
    QString m_chosen;
    bool m_listing = false;
    int m_generation = 0;
    std::shared_ptr<usdcc::assets::Cancel> m_cancel;
    QStringList m_listedFiles;  // the files the shown listing has, by their full path
    std::vector<Listed> m_pages;  // the pages of the folder listed so far
    std::string m_next;           // where its next page starts; empty when that was all

    QComboBox* m_scheme = nullptr;
    QLineEdit* m_path = nullptr;
    QToolButton* m_favorite = nullptr;
    QToolButton* m_up = nullptr;
    QTreeWidget* m_places = nullptr;
    QTreeWidget* m_entries = nullptr;
    QLineEdit* m_fileName = nullptr;
    QComboBox* m_filter = nullptr;
    QRadioButton* m_relative = nullptr;
    QRadioButton* m_absolute = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_saveNote = nullptr;
    QToolButton* m_newFolder = nullptr;
    QStringList m_newFolders;  // on a server, made when something is saved in them
    std::function<bool(const QString&)> m_askReplace;
};

}  // namespace usdcc::usd
