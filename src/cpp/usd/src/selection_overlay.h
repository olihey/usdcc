#pragma once

// The Outline and Wireframe selection styles (docs/VIEWPORT_SELECTION.md §3): a second,
// Storm-only Hydra engine draws the selection into a framebuffer of its own, and two
// shader passes turn that into lines over the viewport's frame. Private to
// HydraViewportWindow, which owns one per window while a line style is in use.

#include "usdcc/usd/hydra_viewport_window.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <QColor>
#include <QSize>

#include <memory>
#include <vector>

class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;

PXR_NAMESPACE_OPEN_SCOPE
class UsdImagingGLEngine;
PXR_NAMESPACE_CLOSE_SCOPE

namespace usdcc::usd {

class SelectionOverlay {
public:
    // Needs the viewport's GL context current, here and in every call below, the
    // destructor included: the engine and the GL objects are freed as it goes.
    SelectionOverlay();
    ~SelectionOverlay();
    SelectionOverlay(const SelectionOverlay&) = delete;
    SelectionOverlay& operator=(const SelectionOverlay&) = delete;

    struct Frame {
        PXR_NS::UsdStageRefPtr stage;
        std::vector<PXR_NS::SdfPath> selection;  // the prims selected, USD paths
        SelectionStyle style = SelectionStyle::Outline;  // Outline or Wireframe
        QColor color;
        double width = 2.0;  // the lines' width in device pixels
        PXR_NS::GfMatrix4d viewMatrix, projMatrix;
        double nearClip = 0.1, farClip = 10000.0;  // of projMatrix, to compare depths
        QSize size;  // the viewport framebuffer's, in device pixels
        unsigned int targetFramebuffer = 0;  // drawn onto; holds Hydra's color and depth
    };
    // Draws the selection's lines onto `frame.targetFramebuffer`, which is bound again
    // afterwards. A stage other than the last one starts a new engine (Hydra populates
    // from one stage only).
    void draw(const Frame& frame);

    // The collection the overlay engine renders for `selection`: the selected paths, and
    // the native instances' propagated prototypes when the selection is, holds or sits
    // inside a native instance (their geometry lives there, not under the instance).
    static std::vector<PXR_NS::SdfPath> renderRoots(const PXR_NS::UsdStageRefPtr& stage,
                                                    const std::vector<PXR_NS::SdfPath>& selection);

private:
    void ensureTargets(const QSize& size);
    void releaseTargets();
    bool ensurePrograms();
    void renderMask(const Frame& frame);
    void composite(const Frame& frame);

    std::unique_ptr<PXR_NS::UsdImagingGLEngine> m_engine;
    PXR_NS::UsdStageRefPtr m_stage;  // the one m_engine populated from
    std::vector<PXR_NS::SdfPath> m_selection;  // the one m_engine has

    // The mask: the overlay engine's color (selected white, the rest black) and depth.
    unsigned int m_maskFbo = 0, m_maskColor = 0, m_maskDepth = 0;
    // A copy of the viewport's depth, to sample.
    unsigned int m_sceneDepthFbo = 0, m_sceneDepth = 0;
    // The first pass's result: the horizontal distances to the nearest line pixels.
    unsigned int m_rowsFbo = 0, m_rows = 0;
    QSize m_size;

    std::unique_ptr<QOpenGLShaderProgram> m_rowsProgram, m_compositeProgram;
    std::unique_ptr<QOpenGLVertexArrayObject> m_vao;
    bool m_programsFailed = false;
};

}  // namespace usdcc::usd
