#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/time_axis.h"

#include <QWidget>

#include <optional>
#include <vector>

namespace usdcc::usd {

// The timeline's ruler (docs/ANIMATION.md §5.5): ticks and frame numbers over a range of
// time, with the playhead at the time shown and the part that plays lit, the rest dimmed.
// Pressing or dragging in it scrubs: it says which time is wanted and leaves moving there
// to whoever listens. Knows no stage.
class USDCC_USD_UI_API TimeRuler : public QWidget {
    Q_OBJECT

public:
    explicit TimeRuler(QWidget* parent = nullptr);

    // The times shown, edge to edge.
    void setRange(const TimeRange& range);
    TimeRange range() const { return m_axis.range(); }
    // The part of the range that plays: what's outside it is dimmed. Scrubbing still
    // reaches all of the range.
    void setPlaybackRange(const TimeRange& range);
    TimeRange playbackRange() const { return m_playbackRange; }
    // The frames a simulation has cached (docs/PHYSICS.md §5.1): a band along the bottom,
    // dimmed when the cache is stale. Nothing: no band.
    void setCachedRange(const std::optional<TimeRange>& range, bool stale);
    std::optional<TimeRange> cachedRange() const { return m_cachedRange; }
    bool cacheStale() const { return m_cacheStale; }
    // Where the playhead is. Outside the range, it isn't drawn.
    void setTime(double time);
    double time() const { return m_time; }

    // Key marks (docs/ANIMATION.md phase 2): the selection's key times, a diamond each.
    void setKeys(std::vector<double> keys);
    const std::vector<double>& keys() const { return m_keys; }

    // Time to pixels, for the width it has now.
    const TimeAxis& axis() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    // A press began a scrub; timeScrubbed() follows at once, and for every move to
    // another time, until the release.
    void scrubStarted();
    // The time under the cursor, kept inside the range: a whole frame, or with Shift
    // held, any time.
    void timeScrubbed(double time);
    void scrubFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void scrubTo(const QPointF& position, bool anyTime);

    mutable TimeAxis m_axis;  // its extent follows the width, set when asked for
    TimeRange m_playbackRange{0.0, 1.0};
    std::optional<TimeRange> m_cachedRange;
    std::vector<double> m_keys;
    bool m_cacheStale = false;
    double m_time = 0.0;
    bool m_scrubbing = false;
    double m_lastScrubbed = 0.0;
};

// A frame number as the timeline writes it: "24", or "24.5" between two frames.
USDCC_USD_UI_API QString formatFrame(double time);

}  // namespace usdcc::usd
