#include "uv_view.h"

#include "usdcc/usd/stage.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/half.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <QApplication>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdcc::uv {

namespace {
using usdcc::usd::PrimvarElements;
using usdcc::usd::PrimvarViewContext;

const char* const kFacesDomain = "faces";

bool twoNumbers(const SdfValueTypeName& type) {
    const TfType scalar = type.GetScalarType().GetType();
    return type.IsArray() && (scalar == TfType::Find<GfVec2f>() || scalar == TfType::Find<GfVec2d>() || scalar == TfType::Find<GfVec2h>());
}

bool laidOut(const TfToken& interpolation) {
    return interpolation == UsdGeomTokens->faceVarying || interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying;
}

QString number(std::size_t n) { return QLocale().toString(qulonglong(n)); }

QString count(std::size_t n, const QString& one, const QString& many) { return number(n) + QLatin1Char(' ') + (n == 1 ? one : many); }

// The checker behind a tile: eight squares a side, in two greys, with a dot of colour in each
// row and column so its direction shows.
QImage makeChecker() {
    QImage image(256, 256, QImage::Format_RGB32);
    QPainter painter(&image);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            painter.fillRect(x * 32, y * 32, 32, 32, (x + y) % 2 ? QColor(70, 70, 74) : QColor(110, 110, 116));
        }
    }
    painter.setPen(Qt::NoPen);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            // Red grows with u, green with v (the image's rows go down as v goes up).
            painter.setBrush(QColor(60 + x * 26, 60 + (7 - y) * 26, 90));
            painter.drawEllipse(QPointF(x * 32 + 16, y * 32 + 16), 4, 4);
        }
    }
    return image;
}

std::string tilePath(const std::string& file, int tile) {
    const std::size_t at = file.find("<UDIM>");
    if (at == std::string::npos) return file;
    std::string path = file;
    path.replace(at, 6, std::to_string(tile));
    return path;
}

float toFloat(const void* data, HioType type, std::size_t i) {
    switch (type) {
        case HioTypeUnsignedByte:
        case HioTypeUnsignedByteSRGB: return float(static_cast<const std::uint8_t*>(data)[i]) / 255.0f;
        case HioTypeSignedByte: return std::max(0.0f, float(static_cast<const std::int8_t*>(data)[i]) / 127.0f);
        case HioTypeUnsignedShort: return float(static_cast<const std::uint16_t*>(data)[i]) / 65535.0f;
        case HioTypeSignedShort: return std::max(0.0f, float(static_cast<const std::int16_t*>(data)[i]) / 32767.0f);
        case HioTypeUnsignedInt: return float(double(static_cast<const std::uint32_t*>(data)[i]) / 4294967295.0);
        case HioTypeInt: return std::max(0.0f, float(double(static_cast<const std::int32_t*>(data)[i]) / 2147483647.0));
        case HioTypeHalfFloat: return float(static_cast<const GfHalf*>(data)[i]);
        case HioTypeFloat: return static_cast<const float*>(data)[i];
        case HioTypeDouble: return float(static_cast<const double*>(data)[i]);
        default: return 0.0f;
    }
}
}  // namespace

bool uvViewApplies(const PrimvarViewContext& context) { return uvPrim(context).has_value(); }

std::optional<SdfPath> uvPrim(const PrimvarViewContext& context) {
    if (!context.stage || context.primvar.empty()) return std::nullopt;
    const UsdStageRefPtr& stage = context.stage->usdStage();
    for (const usdcc::usd::PrimvarSet& set : context.sets) {
        for (const usdcc::usd::PrimvarInfo& info : set.primvars) {
            if (info.name != context.primvar) continue;
            if (twoNumbers(info.type) && laidOut(info.interpolation) && stage && stage->GetPrimAtPath(set.prim).IsA<UsdGeomMesh>()) {
                return set.prim;
            }
            break;
        }
    }
    return std::nullopt;
}

QImage loadUvTexture(const std::string& path) {
    if (path.empty()) return {};
    constexpr int kMaxSide = 2048;
    QImage image;
    if (HioImageSharedPtr hio = HioImage::OpenForReading(path, 0, 0, HioImage::SourceColorSpace::Auto, true)) {
        const HioFormat format = hio->GetFormat();
        const int width = hio->GetWidth(), height = hio->GetHeight();
        const int channels = HioGetComponentCount(format);
        const HioType type = HioGetHioType(format);
        if (width > 0 && height > 0 && channels > 0 && format != HioFormatInvalid) {
            std::vector<unsigned char> buffer(std::size_t(width) * std::size_t(height) * std::size_t(hio->GetBytesPerPixel()));
            HioImage::StorageSpec spec;
            spec.width = width;
            spec.height = height;
            spec.depth = 1;
            spec.format = format;
            spec.flipped = false;
            spec.data = buffer.data();
            if (hio->Read(spec)) {
                // Linear numbers (floats, as from an EXR) are shown with a gamma; bytes are taken as they are.
                const bool linear = type == HioTypeHalfFloat || type == HioTypeFloat || type == HioTypeDouble;
                image = QImage(width, height, QImage::Format_RGBA8888);
                for (int y = 0; y < height; ++y) {
                    uchar* row = image.scanLine(y);
                    for (int x = 0; x < width; ++x) {
                        const std::size_t at = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * std::size_t(channels);
                        float rgba[4] = {0, 0, 0, 1};
                        for (int c = 0; c < std::min(channels, 4); ++c) rgba[c] = toFloat(buffer.data(), type, at + std::size_t(c));
                        if (channels == 1 || channels == 2) {
                            if (channels == 2) rgba[3] = rgba[1];
                            rgba[1] = rgba[2] = rgba[0];
                        }
                        for (int c = 0; c < 4; ++c) {
                            float v = std::clamp(rgba[c], 0.0f, 1.0f);
                            if (linear && c < 3) v = std::pow(v, 1.0f / 2.2f);
                            row[x * 4 + c] = uchar(std::lround(v * 255.0f));
                        }
                    }
                }
            }
        }
    }
    if (image.isNull()) image = QImage(QString::fromStdString(path));
    if (image.isNull()) return {};
    if (image.width() > kMaxSide || image.height() > kMaxSide) image = image.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// --- UvCanvas ---------------------------------------------------------------------------

UvCanvas::UvCanvas(UvView* view) : QWidget(view), m_view(view) {
    setObjectName(QStringLiteral("uvCanvas"));
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(80, 60);
    setAutoFillBackground(false);
}

QPointF UvCanvas::toScreen(const GfVec2f& uv) const { return {m_origin.x() + uv[0] * m_scale, m_origin.y() - uv[1] * m_scale}; }

GfVec2f UvCanvas::toUv(const QPointF& pos) const {
    return GfVec2f(float((pos.x() - m_origin.x()) / m_scale), float((m_origin.y() - pos.y()) / m_scale));
}

void UvCanvas::fit() {
    GfRange2f range(GfVec2f(0), GfVec2f(1));
    if (const UvLayout* layout = m_view->layout(); layout && !layout->bounds.IsEmpty()) range.UnionWith(layout->bounds);
    const GfVec2f size = range.GetSize();
    const double margin = 24;
    const double w = std::max(1.0, width() - 2 * margin), h = std::max(1.0, height() - 2 * margin);
    m_scale = std::clamp(std::min(w / std::max(size[0], 1e-6f), h / std::max(size[1], 1e-6f)), 1e-3, 1e7);
    const GfVec2f middle = range.GetMidpoint();
    m_origin = QPointF(width() / 2.0 - middle[0] * m_scale, height() / 2.0 + middle[1] * m_scale);
    m_fitted = true;
    m_moved = false;
    update();
}

void UvCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Fitted, it stays fitted as the panel's size changes.
    if (!m_moved) fit();
}

void UvCanvas::drawBackground(QPainter& painter) {
    const UvLayout* layout = m_view->layout();
    if (m_view->background() == UvView::Background::None) return;
    std::vector<int> tiles = layout && !layout->udimTiles.empty() ? layout->udimTiles : std::vector<int>{1001};
    painter.save();
    painter.setOpacity(0.8);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const int tile : tiles) {
        const QImage* image = m_view->tileImage(tile);
        if (!image || image->isNull()) continue;
        const float u = float((tile - 1001) % 10), v = float((tile - 1001) / 10);
        const QRectF rect(toScreen(GfVec2f(u, v + 1)), toScreen(GfVec2f(u + 1, v)));
        painter.drawImage(rect, *image);
    }
    painter.restore();
}

void UvCanvas::drawGrid(QPainter& painter) {
    const GfVec2f low = toUv(QPointF(0, height())), high = toUv(QPointF(width(), 0));
    // Lines about every 40 pixels or more: a power of ten of UV units, halved where that's closer.
    double step = std::pow(10.0, std::ceil(std::log10(40.0 / m_scale)));
    if (step * m_scale / 2 >= 40) step /= 2;
    const auto lines = [&](double every, const QColor& color, qreal width) {
        if (every * m_scale < 4) return;
        QVector<QLineF> drawn;
        for (double u = std::floor(low[0] / every) * every; u <= high[0]; u += every) {
            drawn.append(QLineF(toScreen(GfVec2f(float(u), low[1])), toScreen(GfVec2f(float(u), high[1]))));
        }
        for (double v = std::floor(low[1] / every) * every; v <= high[1]; v += every) {
            drawn.append(QLineF(toScreen(GfVec2f(low[0], float(v))), toScreen(GfVec2f(high[0], float(v)))));
        }
        painter.setPen(QPen(color, width));
        painter.drawLines(drawn);
    };
    const QColor text = palette().color(QPalette::WindowText);
    QColor fine = text;
    fine.setAlpha(22);
    QColor whole = text;
    whole.setAlpha(60);
    if (step < 1) lines(step, fine, 0);
    lines(1.0, whole, 1);

    // The 0-1 square, and the UDIM tiles the layout uses with their numbers.
    const UvLayout* layout = m_view->layout();
    std::set<int> tiles{1001};
    if (layout) tiles.insert(layout->udimTiles.begin(), layout->udimTiles.end());
    QFont font = painter.font();
    font.setPointSizeF(std::clamp(m_scale / 18.0, 6.0, 11.0));
    painter.setFont(font);
    for (const int tile : tiles) {
        const float u = float((tile - 1001) % 10), v = float((tile - 1001) / 10);
        const QRectF rect(toScreen(GfVec2f(u, v + 1)), toScreen(GfVec2f(u + 1, v)));
        QColor border = tile == 1001 ? QColor(232, 180, 60) : text;
        border.setAlpha(tile == 1001 ? 200 : 120);
        painter.setPen(QPen(border, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
        if (m_scale >= 60 && (tile != 1001 || tiles.size() > 1)) {
            painter.setPen(border);
            painter.drawText(rect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop, QString::number(tile));
        }
    }
}

void UvCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QColor base = palette().color(QPalette::Base);
    painter.fillRect(rect(), base);
    drawBackground(painter);
    drawGrid(painter);

    const UvLayout* layout = m_view->layout();
    if (!layout || !layout->error.empty() || layout->faces() == 0) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        const QString why = layout && !layout->error.empty() ? QString::fromStdString(layout->error)
                            : m_view->isComputing()            ? tr("Laying it out...")
                                                               : QString::fromStdString(m_view->m_why);
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, why);
        return;
    }

    const QTransform uvToScreen(m_scale, 0, 0, -m_scale, m_origin.x(), m_origin.y());
    const auto polygon = [&](std::size_t face) {
        QPolygonF points;
        for (int i = 0; i < layout->faceCounts[face]; ++i) points << toScreen(layout->corner(face, i));
        return points;
    };
    const bool many = m_view->edgeLines().size() > 60000;
    painter.setRenderHint(QPainter::Antialiasing, !many);

    // Problems, then what's picked, under the edges.
    painter.setPen(Qt::NoPen);
    if (m_view->showsProblems()) {
        painter.setBrush(QColor(230, 60, 60, 120));
        for (std::size_t f = 0; f < layout->faces(); ++f) {
            if (layout->flags[f] & UvLayout::Flipped) painter.drawPolygon(polygon(f));
        }
    }
    const std::vector<std::uint8_t>& picked = m_view->pickedFaces();
    painter.setBrush(QColor(255, 200, 40, 110));
    for (std::size_t f = 0; f < picked.size() && f < layout->faces(); ++f) {
        if (picked[f]) painter.drawPolygon(polygon(f));
    }

    // Every edge at once, in UV with a hairline pen; then the seams over them.
    painter.save();
    painter.setTransform(uvToScreen, true);
    QColor edge = palette().color(QPalette::WindowText);
    edge.setAlpha(170);
    QPen edgePen(edge, 0);
    edgePen.setCosmetic(true);
    painter.setPen(edgePen);
    painter.drawLines(m_view->edgeLines());
    if (m_view->showsSeams()) {
        QPen seamPen(QColor(40, 200, 230), 2);
        seamPen.setCosmetic(true);
        painter.setPen(seamPen);
        painter.drawLines(m_view->seamLines());
    }
    painter.restore();

    if (m_view->showsProblems() && layout->degenerate) {
        painter.setPen(QPen(QColor(255, 140, 30), 2));
        painter.setBrush(Qt::NoBrush);
        for (std::size_t f = 0; f < layout->faces(); ++f) {
            if ((layout->flags[f] & UvLayout::Degenerate) && layout->faceCounts[f] > 0) painter.drawEllipse(toScreen(layout->corner(f, 0)), 5, 5);
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 200, 40));
    for (const std::size_t corner : m_view->pickedCorners()) painter.drawEllipse(toScreen(layout->corners[corner]), 3.5, 3.5);

    if (const std::optional<std::size_t> hovered = m_view->hoveredFace()) {
        painter.setPen(QPen(Qt::white, 2));
        painter.setBrush(QColor(255, 255, 255, 50));
        painter.drawPolygon(polygon(*hovered));
    }
}

void UvCanvas::mousePressEvent(QMouseEvent* event) {
    using usdcc::ui::NavigationAction;
    event->accept();
    // One drag at a time: another button during it isn't another.
    if (m_navigation.dragging() != NavigationAction::None && (event->buttons() & ~event->button())) return;
    m_navigation.cancel();
    m_pressAt = m_lastAt = event->position();
    // What Settings > Navigation binds pans and zooms. A left drag pans too (below): it
    // has nothing else to do here.
    m_navigation.press(event);
}

void UvCanvas::mouseMoveEvent(QMouseEvent* event) {
    using usdcc::ui::NavigationAction;
    const QPointF at = event->position();
    // A left drag pans, once it's more than a nudge.
    if (m_navigation.dragging() == NavigationAction::None && (event->buttons() & Qt::LeftButton) &&
        (at - m_pressAt).manhattanLength() > 4) {
        m_navigation.begin(NavigationAction::Pan, Qt::LeftButton, m_lastAt);
    }
    if (m_navigation.dragging() != NavigationAction::None) {
        navigate(m_navigation.move(event));
        m_lastAt = at;
        return;
    }
    m_lastAt = at;
    m_view->hoverAt(toUv(at));
}

void UvCanvas::mouseReleaseEvent(QMouseEvent* event) {
    const bool navigated = m_navigation.release(event);
    if (!navigated && event->button() == Qt::LeftButton) {
        m_view->pickAt(toUv(event->position()), event->modifiers() & Qt::ShiftModifier, event->modifiers() & Qt::ControlModifier);
    }
}

void UvCanvas::wheelEvent(QWheelEvent* event) {
    navigate(m_navigation.wheel(event));
    event->accept();
}

bool UvCanvas::event(QEvent* event) {
    // The key that frames is this view's while it has the focus.
    if (usdcc::ui::claimFrameKey(event)) return true;
    // A pinch zooms, as the wheel does.
    if (const usdcc::ui::NavigationStep step = m_navigation.gesture(event)) {
        navigate(step);
        return true;
    }
    return QWidget::event(event);
}

void UvCanvas::navigate(const usdcc::ui::NavigationStep& step) {
    switch (step.action) {
        case usdcc::ui::NavigationAction::Pan: m_origin += step.delta; break;
        case usdcc::ui::NavigationAction::Zoom: {
            if (step.zoom == 0.0) return;
            const GfVec2f uv = toUv(step.anchor);
            m_scale = std::clamp(m_scale / usdcc::ui::navigationZoomFactor(step.zoom), 1e-3, 1e7);
            // The point under the cursor, or the one the drag began on, stays put.
            m_origin = QPointF(step.anchor.x() - uv[0] * m_scale, step.anchor.y() + uv[1] * m_scale);
            m_fitted = true;
            break;
        }
        default: return;
    }
    m_moved = true;
    update();
}

void UvCanvas::keyPressEvent(QKeyEvent* event) {
    if (usdcc::ui::isFrameKey(event)) {
        fit();
        return;
    }
    QWidget::keyPressEvent(event);
}

void UvCanvas::leaveEvent(QEvent* event) {
    m_view->clearHover();
    QWidget::leaveEvent(event);
}

// --- UvView ----------------------------------------------------------------------------

UvView::UvView(QWidget* parent) : PrimvarView(parent), m_checker(makeChecker()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* bar = new QWidget(this);
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(4, 2, 4, 2);
    barLayout->addWidget(new QLabel(tr("Behind:"), bar));
    m_backgroundChoice = new QComboBox(bar);
    m_backgroundChoice->setObjectName(QStringLiteral("uvBackground"));
    m_backgroundChoice->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_backgroundChoice, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updatingMenu || index < 0) return;
        const int value = m_backgroundChoice->itemData(index).toInt();
        if (value < 0) {
            setBackground(value == -1 ? Background::None : Background::Checker);
        } else {
            setBackground(Background::Texture, value);
        }
    });
    barLayout->addWidget(m_backgroundChoice);
    const auto toggle = [&](const QString& name, const QString& text, const QString& tip) {
        auto* button = new QToolButton(bar);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setChecked(true);
        button->setAutoRaise(true);
        barLayout->addWidget(button);
        return button;
    };
    m_seamsButton = toggle(QStringLiteral("uvSeams"), tr("Seams"), tr("Edges where the mesh is one piece but its UVs aren't"));
    connect(m_seamsButton, &QToolButton::toggled, this, &UvView::setShowsSeams);
    m_problemsButton = toggle(QStringLiteral("uvProblems"), tr("Problems"), tr("Flipped faces in red, degenerate ones circled"));
    connect(m_problemsButton, &QToolButton::toggled, this, &UvView::setShowsProblems);
    auto* fit = new QToolButton(bar);
    fit->setObjectName(QStringLiteral("uvFit"));
    fit->setText(tr("Fit"));
    fit->setToolTip(tr("The layout and the 0-1 square in view (F)"));
    fit->setAutoRaise(true);
    barLayout->addWidget(fit);
    barLayout->addStretch(1);
    layout->addWidget(bar);

    m_canvas = new UvCanvas(this);
    layout->addWidget(m_canvas, 1);
    connect(fit, &QToolButton::clicked, m_canvas, &UvCanvas::fit);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("uvSummary"));
    m_summary->setWordWrap(true);
    m_summary->setContentsMargins(6, 2, 6, 4);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(m_summary);

    fillBackgroundMenu();
    updateSummary();
}

UvView::~UvView() {
    ++*m_generation;  // a layout still being worked out is dropped
    if (m_stage && m_layout && m_stage->primvarElementHover().prim == m_layout->prim) m_stage->setPrimvarElementHover({});
}

void UvView::setContext(const PrimvarViewContext& context, bool focusOnly) {
    if (context.stage != m_stage) {
        m_stage = context.stage;
        m_elementsConnection = m_stage ? m_stage->primvarElementsChanged.connect([this]() { showStageElements(); })
                                       : usdcc::core::Signal<>::Connection();
        m_hoverConnection = m_stage ? m_stage->primvarElementHoverChanged.connect([this]() { m_canvas->update(); })
                                    : usdcc::core::Signal<>::Connection();
    }
    const std::optional<SdfPath> prim = uvPrim(context);
    const std::string key = prim ? prim->GetString() + "\n" + context.primvar + "\n" + std::to_string(context.time.GetValue()) : std::string();
    // Only the focus moved, to what's shown already.
    if (focusOnly && key == m_key && !key.empty()) return;
    readTextures(context);
    if (key != m_key) m_canvas->m_fitted = false;
    m_key = key;
    compute(context);
}

void UvView::compute(const PrimvarViewContext& context) {
    const int generation = ++*m_generation;
    const std::optional<SdfPath> prim = uvPrim(context);
    if (!prim) {
        m_layout.reset();
        m_edgeLines.clear();
        m_seamLines.clear();
        m_computing = false;
        m_why = context.primvar.empty() ? tr("Pick a UV set in the list.").toStdString()
                                        : tr("None of the selected meshes has %1 as UVs: two numbers a face corner or a point.")
                                              .arg(QString::fromStdString(context.primvar))
                                              .toStdString();
        showStageElements();
        updateSummary();
        m_canvas->update();
        Q_EMIT layoutReady();
        return;
    }
    // Read here, worked out on a worker.
    const UvSource source = readUvSource(context.stage->usdStage()->GetPrimAtPath(*prim), context.primvar, context.time);
    m_computing = true;
    updateSummary();
    QPointer<UvView> self(this);
    std::shared_ptr<int> generations = m_generation;
    QThreadPool::globalInstance()->start([source, self, generations, generation]() {
        auto layout = std::make_shared<UvLayout>(computeUvLayout(source));
        QMetaObject::invokeMethod(
            qApp,
            [self, generations, generation, layout]() {
                if (!self || *generations != generation) return;
                self->layoutComputed(layout);
            },
            Qt::QueuedConnection);
    });
}

void UvView::layoutComputed(std::shared_ptr<UvLayout> layout) {
    m_computing = false;
    m_layout = std::move(layout);
    m_edgeLines.clear();
    m_seamLines.clear();
    const auto line = [this](std::size_t a, std::size_t b) {
        const GfVec2f& p = m_layout->corners[a];
        const GfVec2f& q = m_layout->corners[b];
        return QLineF(p[0], p[1], q[0], q[1]);
    };
    if (m_layout->error.empty()) {
        m_edgeLines.reserve(qsizetype(m_layout->corners.size()));
        for (std::size_t f = 0; f < m_layout->faces(); ++f) {
            const int n = m_layout->faceCounts[f];
            for (int i = 0; i < n; ++i) {
                m_edgeLines.append(line(m_layout->faceStarts[f] + std::size_t(i), m_layout->faceStarts[f] + std::size_t((i + 1) % n)));
            }
        }
        for (const auto& [a, b] : m_layout->seams) m_seamLines.append(line(a, b));
    }
    if (!m_canvas->m_fitted) m_canvas->fit();
    showStageElements();
    updateSummary();
    loadTexture();
    m_canvas->update();
    Q_EMIT layoutReady();
}

void UvView::readTextures(const PrimvarViewContext& context) {
    std::vector<UvTexture> textures;
    if (const std::optional<SdfPath> prim = uvPrim(context)) textures = texturesReading(context.stage->usdStage()->GetPrimAtPath(*prim), context.primvar);
    const bool same = textures.size() == m_textures.size() &&
                      std::equal(textures.begin(), textures.end(), m_textures.begin(),
                                 [](const UvTexture& a, const UvTexture& b) { return a.shader == b.shader && a.file == b.file; });
    if (same) return;
    m_textures = std::move(textures);
    fillBackgroundMenu();
}

void UvView::fillBackgroundMenu() {
    m_updatingMenu = true;
    m_backgroundChoice->clear();
    m_backgroundChoice->addItem(tr("Nothing"), -1);
    m_backgroundChoice->addItem(tr("Checker"), -2);
    for (std::size_t i = 0; i < m_textures.size(); ++i) {
        const QString file = QFileInfo(QString::fromStdString(m_textures[i].file)).fileName();
        m_backgroundChoice->addItem(QStringLiteral("%1 (%2)").arg(file, QString::fromStdString(m_textures[i].shader.GetName())), int(i));
        m_backgroundChoice->setItemData(m_backgroundChoice->count() - 1, QString::fromStdString(m_textures[i].file), Qt::ToolTipRole);
    }
    // A texture that's gone shows the checker.
    if (m_background == Background::Texture && m_textures.empty()) {
        m_backgroundChoice->setCurrentIndex(1);
    } else {
        m_backgroundChoice->setCurrentIndex(m_background == Background::None      ? 0
                                            : m_background == Background::Checker ? 1
                                                                                  : 2 + int(textureShown()));
    }
    m_updatingMenu = false;
}

void UvView::setBackground(Background background, int texture) {
    m_background = background;
    if (background == Background::Texture) m_textureIndex = std::max(0, texture);
    fillBackgroundMenu();
    loadTexture();
    m_canvas->update();
}

const QImage* UvView::tileImage(int tile) const {
    if (m_background == Background::Checker || (m_background == Background::Texture && m_textures.empty())) return &m_checker;
    if (m_background != Background::Texture) return nullptr;
    const std::string& file = m_textures[textureShown()].file;
    const bool udim = file.find("<UDIM>") != std::string::npos;
    if (!udim && tile != 1001) return nullptr;
    const auto it = m_images.constFind(QString::fromStdString(file) + QLatin1Char('|') + QString::number(udim ? tile : 1001));
    return it == m_images.cend() || it->isNull() ? nullptr : &*it;
}

void UvView::loadTexture() {
    if (m_background != Background::Texture || m_textures.empty()) return;
    const std::string file = m_textures[textureShown()].file;
    const bool udim = file.find("<UDIM>") != std::string::npos;
    std::vector<int> tiles = udim && m_layout && !m_layout->udimTiles.empty() ? m_layout->udimTiles : std::vector<int>{1001};
    QPointer<UvView> self(this);
    for (const int tile : tiles) {
        const QString key = QString::fromStdString(file) + QLatin1Char('|') + QString::number(tile);
        if (m_images.contains(key)) continue;
        m_images.insert(key, QImage());  // being read: not read again
        ++m_loading;
        const std::string path = tilePath(file, tile);
        QThreadPool::globalInstance()->start([self, key, path]() {
            const QImage image = loadUvTexture(path);
            QMetaObject::invokeMethod(
                qApp,
                [self, key, image]() {
                    if (!self) return;
                    self->m_images.insert(key, image);
                    --self->m_loading;
                    self->m_canvas->update();
                    Q_EMIT self->textureLoaded();
                },
                Qt::QueuedConnection);
        });
    }
}

std::size_t UvView::textureShown() const { return std::min(std::size_t(m_textureIndex), m_textures.empty() ? 0 : m_textures.size() - 1); }

void UvView::setShowsSeams(bool on) {
    m_seams = on;
    if (m_seamsButton->isChecked() != on) m_seamsButton->setChecked(on);
    m_canvas->update();
}

void UvView::setShowsProblems(bool on) {
    m_problems = on;
    if (m_problemsButton->isChecked() != on) m_problemsButton->setChecked(on);
    m_canvas->update();
}

std::optional<std::size_t> UvView::hoveredFace() const {
    if (!m_stage || !m_layout || !m_layout->error.empty()) return std::nullopt;
    const PrimvarElements& hover = m_stage->primvarElementHover();
    if (hover.prim != m_layout->prim || hover.domain != kFacesDomain || hover.indices.size() != 1 ||
        hover.indices.front() >= m_layout->faces()) {
        return std::nullopt;
    }
    return hover.indices.front();
}

void UvView::hoverAt(const GfVec2f& uv) {
    if (!m_stage || !m_layout || !m_layout->error.empty()) return;
    const std::optional<std::size_t> face = m_layout->faceAt(uv);
    if (face == hoveredFace()) return;
    if (face) {
        m_stage->setPrimvarElementHover({m_layout->prim, kFacesDomain, {*face}});
    } else {
        clearHover();
    }
}

void UvView::clearHover() {
    if (m_stage && m_layout && m_stage->primvarElementHover().prim == m_layout->prim) m_stage->setPrimvarElementHover({});
}

void UvView::pickAt(const GfVec2f& uv, bool add, bool toggle) {
    if (!m_stage || !m_layout || !m_layout->error.empty()) return;
    const std::optional<std::size_t> face = m_layout->faceAt(uv);
    const PrimvarElements& current = m_stage->primvarElements();
    std::set<std::size_t> faces;
    if ((add || toggle) && current.prim == m_layout->prim && current.domain == kFacesDomain) faces.insert(current.indices.begin(), current.indices.end());
    if (face) {
        if (toggle && faces.count(*face)) {
            faces.erase(*face);
        } else {
            faces.insert(*face);
        }
    } else if (!add && !toggle) {
        faces.clear();
    }
    if (faces.empty()) {
        // Nothing picked here takes away only what this mesh had.
        if (current.prim == m_layout->prim) m_stage->setPrimvarElements({});
        return;
    }
    m_stage->setPrimvarElements({m_layout->prim, kFacesDomain, std::vector<std::size_t>(faces.begin(), faces.end())});
}

void UvView::showStageElements() {
    m_pickedFaces.clear();
    m_pickedCorners.clear();
    if (m_stage && m_layout && m_layout->error.empty()) {
        const PrimvarElements& elements = m_stage->primvarElements();
        if (elements.prim == m_layout->prim) {
            if (elements.domain == kFacesDomain) {
                m_pickedFaces.assign(m_layout->faces(), 0);
                for (const std::size_t f : elements.indices) {
                    if (f < m_layout->faces()) m_pickedFaces[f] = 1;
                }
            } else if (elements.domain == "face corners") {
                for (const std::size_t c : elements.indices) {
                    if (c < m_layout->corners.size()) m_pickedCorners.push_back(c);
                }
            } else if (elements.domain == "points") {
                const std::set<std::size_t> points(elements.indices.begin(), elements.indices.end());
                for (std::size_t c = 0; c < m_layout->cornerPoints.size(); ++c) {
                    if (m_layout->cornerPoints[c] >= 0 && points.count(std::size_t(m_layout->cornerPoints[c]))) m_pickedCorners.push_back(c);
                }
            }
        }
    }
    m_canvas->update();
}

QString UvView::summary() const { return m_summary->text(); }

void UvView::updateSummary() {
    if (!m_layout) {
        m_summary->setText(m_computing ? tr("Laying it out...") : QString::fromStdString(m_why));
        return;
    }
    if (!m_layout->error.empty()) {
        m_summary->setText(QString::fromStdString(m_layout->error));
        return;
    }
    QStringList parts;
    parts << QStringLiteral("%1 · %2, %3").arg(QString::fromStdString(m_layout->prim.GetName()), QString::fromStdString(m_layout->primvar),
                                              QString::fromStdString(m_layout->interpolation.GetString()));
    parts << count(m_layout->faces(), tr("face"), tr("faces"));
    if (m_layout->udimTiles.size() > 1 || (m_layout->udimTiles.size() == 1 && m_layout->udimTiles.front() != 1001)) {
        QStringList tiles;
        for (const int tile : m_layout->udimTiles) tiles << QString::number(tile);
        parts << tr("UDIM tiles %1").arg(tiles.join(QStringLiteral(", ")));
    } else if (m_layout->outside) {
        parts << tr("%1 outside 0-1").arg(number(m_layout->outside));
    }
    if (m_layout->flipped) parts << tr("%1 flipped").arg(number(m_layout->flipped));
    if (m_layout->degenerate) parts << tr("%1 degenerate").arg(number(m_layout->degenerate));
    if (!m_layout->seams.empty()) parts << count(m_layout->seams.size() / 2, tr("seam edge"), tr("seam edges"));
    if (m_layout->badIndices) parts << count(m_layout->badIndices, tr("corner without a UV"), tr("corners without a UV"));
    m_summary->setText(parts.join(QStringLiteral(" · ")));
}

QJsonObject UvView::saveState() const {
    QJsonObject state;
    state[QStringLiteral("background")] = m_background == Background::None      ? QStringLiteral("none")
                                          : m_background == Background::Checker ? QStringLiteral("checker")
                                                                                : QStringLiteral("texture");
    state[QStringLiteral("texture")] = m_textureIndex;
    state[QStringLiteral("seams")] = m_seams;
    state[QStringLiteral("problems")] = m_problems;
    return state;
}

void UvView::restoreState(const QJsonObject& state) {
    const QString background = state.value(QStringLiteral("background")).toString(QStringLiteral("checker"));
    m_textureIndex = state.value(QStringLiteral("texture")).toInt(0);
    setBackground(background == QLatin1String("none")      ? Background::None
                  : background == QLatin1String("texture") ? Background::Texture
                                                            : Background::Checker,
                  m_textureIndex);
    setShowsSeams(state.value(QStringLiteral("seams")).toBool(true));
    setShowsProblems(state.value(QStringLiteral("problems")).toBool(true));
}

}  // namespace usdcc::uv
