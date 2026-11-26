#pragma once

// The UV view (docs/PRIMVAR_VIEWER.md §6, §18): the focused UV set of the first selected mesh that
// has it, laid out flat: the 0-1 square and the UDIM tiles it uses, a checker or a texture the
// bound material reads with it behind, its faces' edges and seams, flipped faces in red and
// degenerate ones marked. The face under the cursor is the stage's element hover and a click picks
// faces (Stage::primvarElements()), so the viewport's overlay shows them on the mesh; the stage's
// elements show here too. A view of the Primvars panel, added by the UV View plugin.

#include "uv_layout.h"

#include "usdcc/usd/primvar_views.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QLineF>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QToolButton;

namespace usdcc::uv {

// The id the plugin registers the view with; its panel type is "primvarView.<id>".
inline constexpr const char* kUvViewId = "org.usdcc.uv-view.uv";

// Whether the view applies to a context: its focus is two numbers a value, on a selected mesh,
// faceVarying, vertex or varying.
bool uvViewApplies(const usdcc::usd::PrimvarViewContext& context);
// The selected mesh it shows, the first that has the focus as uvViewApplies() wants it.
std::optional<PXR_NS::SdfPath> uvPrim(const usdcc::usd::PrimvarViewContext& context);

// A texture as the view draws it: its file read with HioImage (or Qt's readers), at most 2048
// pixels a side. Null if it can't be read.
QImage loadUvTexture(const std::string& path);

class UvView;

// Where the layout is drawn: pan with a drag, zoom with the wheel, F to fit.
class UvCanvas : public QWidget {
    Q_OBJECT

public:
    explicit UvCanvas(UvView* view);

    // UV to the widget's pixels and back: v goes up.
    QPointF toScreen(const PXR_NS::GfVec2f& uv) const;
    PXR_NS::GfVec2f toUv(const QPointF& pos) const;
    // The layout and the 0-1 square in view, with a margin.
    void fit();
    double scale() const { return m_scale; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void drawGrid(QPainter& painter);
    void drawBackground(QPainter& painter);

    UvView* m_view;
    double m_scale = 200;  // pixels for one UV unit
    QPointF m_origin;      // where UV (0, 0) is
    bool m_fitted = false;  // false: fit() when the next layout comes
    bool m_moved = false;   // panned or zoomed: a new size keeps the view where it is
    bool m_panning = false;
    QPointF m_pressAt, m_lastAt;

    friend class UvView;
};

class UvView : public usdcc::usd::PrimvarView {
    Q_OBJECT

public:
    enum class Background { None, Checker, Texture };

    explicit UvView(QWidget* parent = nullptr);
    ~UvView() override;
    void setContext(const usdcc::usd::PrimvarViewContext& context, bool focusOnly) override;
    QJsonObject saveState() const override;
    void restoreState(const QJsonObject& state) override;

    // What's shown; null until the first layout's worked out.
    const UvLayout* layout() const { return m_layout.get(); }
    bool isComputing() const { return m_computing; }
    // The line under the layout: the prim, its faces, tiles and problems, or why there's nothing.
    QString summary() const;
    UvCanvas* canvas() const { return m_canvas; }

    Background background() const { return m_background; }
    // The textures the bound material reads with this set, as the background menu lists them.
    const std::vector<UvTexture>& textures() const { return m_textures; }
    // The texture chosen, which may be past the end until the textures are read; the one shown.
    int textureIndex() const { return m_textureIndex; }
    std::size_t textureShown() const;
    void setBackground(Background background, int texture = 0);
    // The pictures behind the tiles: for a texture, a tile's once it's read (1001 for one without
    // <UDIM>); for the checker, any tile's.
    const QImage* tileImage(int tile) const;
    bool isLoadingTexture() const { return m_loading > 0; }

    bool showsSeams() const { return m_seams; }
    void setShowsSeams(bool on);
    bool showsProblems() const { return m_problems; }
    void setShowsProblems(bool on);

    // The face the stage's hover is on, and the faces and corners its element selection covers
    // here: faces for faces, corners for points and face corners.
    std::optional<std::size_t> hoveredFace() const;
    const std::vector<std::uint8_t>& pickedFaces() const { return m_pickedFaces; }
    const std::vector<std::size_t>& pickedCorners() const { return m_pickedCorners; }

    // What the canvas does with the mouse, here for tests: the face at a UV becomes the hover;
    // a click picks it (adding to what's picked, or taking it away when toggling) or, off every
    // face, picks nothing.
    void hoverAt(const PXR_NS::GfVec2f& uv);
    void clearHover();
    void pickAt(const PXR_NS::GfVec2f& uv, bool add, bool toggle);

    // Lines to draw, in UV: every face's edges, and the seams'.
    const QVector<QLineF>& edgeLines() const { return m_edgeLines; }
    const QVector<QLineF>& seamLines() const { return m_seamLines; }

Q_SIGNALS:
    void layoutReady();
    void textureLoaded();

private:
    void compute(const usdcc::usd::PrimvarViewContext& context);
    void layoutComputed(std::shared_ptr<UvLayout> layout);
    void readTextures(const usdcc::usd::PrimvarViewContext& context);
    void loadTexture();
    void showStageElements();
    void fillBackgroundMenu();
    void updateSummary();

    usdcc::usd::StageRefPtr m_stage;
    usdcc::core::Signal<>::Connection m_elementsConnection;
    usdcc::core::Signal<>::Connection m_hoverConnection;
    std::shared_ptr<int> m_generation = std::make_shared<int>(0);
    std::string m_key;  // the prim, primvar and time of m_layout, to keep the view on them
    bool m_computing = false;
    std::shared_ptr<UvLayout> m_layout;
    std::string m_why;  // why there's no layout, before one's computed
    QVector<QLineF> m_edgeLines, m_seamLines;
    std::vector<std::uint8_t> m_pickedFaces;
    std::vector<std::size_t> m_pickedCorners;

    Background m_background = Background::Checker;
    std::vector<UvTexture> m_textures;
    int m_textureIndex = 0;
    QHash<QString, QImage> m_images;  // "file|tile" to what was read; null when it couldn't be
    int m_loading = 0;
    QImage m_checker;
    bool m_seams = true;
    bool m_problems = true;

    QComboBox* m_backgroundChoice;
    QToolButton* m_seamsButton;
    QToolButton* m_problemsButton;
    UvCanvas* m_canvas;
    QLabel* m_summary;
    bool m_updatingMenu = false;

    friend class UvCanvas;
};

}  // namespace usdcc::uv
