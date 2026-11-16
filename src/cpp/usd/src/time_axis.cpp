#include "usdcc/usd/time_axis.h"

#include <algorithm>
#include <cmath>

namespace usdcc::usd {

namespace {
constexpr double kShortestRange = 0.1;  // time codes across the extent, zoomed all the way in

// 1, 2, 5, 10, 20, 50, ...: the first that is at least `least`.
double niceStepFrom(double least) {
    double step = 1.0;
    static constexpr double kFactors[] = {2.0, 2.5, 2.0};  // 1 -> 2 -> 5 -> 10
    for (int i = 0; step < least && i < 60; ++i) step *= kFactors[i % 3];
    return step;
}
}  // namespace

void TimeAxis::setRange(const TimeRange& range) {
    m_range = ordered(range);
    if (m_range.length() <= 0.0) m_range.end = m_range.start + 1.0;
}

void TimeAxis::setExtent(double left, double right) {
    m_left = left;
    m_right = std::max(right, left + 1.0);
}

double TimeAxis::pixelsPerTimeCode() const { return (m_right - m_left) / m_range.length(); }

double TimeAxis::pixelAt(double time) const { return m_left + (time - m_range.start) * pixelsPerTimeCode(); }

double TimeAxis::timeAt(double pixel) const { return m_range.start + (pixel - m_left) / pixelsPerTimeCode(); }

double TimeAxis::frameAt(double pixel) const { return snappedToFrame(timeAt(pixel)); }

void TimeAxis::zoom(double factor, double anchorPixel) {
    if (!(factor > 0.0) || !std::isfinite(factor)) return;
    const double anchor = timeAt(anchorPixel);
    const double length = std::max(m_range.length() * factor, kShortestRange);
    // The anchor keeps its share of the way across.
    const double share = (anchorPixel - m_left) / (m_right - m_left);
    m_range.start = anchor - share * length;
    m_range.end = m_range.start + length;
}

void TimeAxis::pan(double pixels) {
    const double shift = -pixels / pixelsPerTimeCode();
    m_range.start += shift;
    m_range.end += shift;
}

TimeAxis::TickSteps TimeAxis::tickSteps(double labelPixels, double tickPixels) const {
    const double perTimeCode = pixelsPerTimeCode();
    TickSteps steps;
    steps.major = niceStepFrom(labelPixels / perTimeCode);
    // The first step far enough apart that the labelled ones fall on: 2 doesn't divide 5.
    steps.minor = steps.major;
    for (double step = niceStepFrom(tickPixels / perTimeCode); step < steps.major; step = niceStepFrom(step * 1.5)) {
        if (std::fmod(steps.major, step) == 0.0) {
            steps.minor = step;
            break;
        }
    }
    return steps;
}

std::vector<double> TimeAxis::ticks(double step) const {
    std::vector<double> times;
    if (!(step > 0.0)) return times;
    // A little slack, so an end that is a tick but a hair off still gets one.
    const double slack = step * 1e-9;
    const double first = std::ceil((m_range.start - slack) / step) * step;
    for (double time = first; time <= m_range.end + slack && times.size() < 100000; time += step) times.push_back(time);
    return times;
}

}  // namespace usdcc::usd
