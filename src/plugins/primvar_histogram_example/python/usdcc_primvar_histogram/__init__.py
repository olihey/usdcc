"""An example plugin (docs/PRIMVAR_VIEWER.md section 9): a view of the Primvars panel, in Python.

It draws a histogram of the primvar in focus: of its numbers, or of one component of a vector
(x, y, z, w) or of its length. The view is a tab under the Primvars panel's list where the
primvar has numbers, and a panel type of its own, "Primvar Histogram".

What it shows how to do:

- subclass ``usdcc.ui.PrimvarView`` and register it with ``usdcc.primvars.register_view()``;
- say where it applies, from the ``ViewContext`` it's given;
- read the values through the context (``focused_values()``), which numpy takes whole when
  it's there, and fall back to Python when it isn't;
- keep its options in sessions (``save_state()``, ``restore_state()``).
"""

from __future__ import annotations

import math
from typing import TYPE_CHECKING, Any

from PySide6 import QtCore, QtGui, QtWidgets

import usdcc.primvars
import usdcc.ui

if TYPE_CHECKING:
    from collections.abc import Sequence

try:
    import numpy
except ImportError:  # the histogram is worked out in Python, more slowly
    numpy = None

VIEW_SUFFIX = ".histogram"  # after the plugin's id, as the view's
BINS = (16, 32, 64, 128)
# Element types with numbers, by the start of their name ("float3[]", "texCoord2f[]", ...).
_NUMERIC = ("float", "double", "half", "int", "uint", "uchar", "bool", "color", "point", "normal", "vector", "texCoord")


def has_numbers(context: usdcc.primvars.ViewContext) -> bool:
    """Where the view's tab shows: the primvar in focus has values, and they're numbers."""
    primvar = context.focused()
    return primvar is not None and primvar.has_value and primvar.type.startswith(_NUMERIC)


def histogram(values: Any, component: int, bins: int) -> tuple[list[int], float, float, int]:
    """Counts for `bins` equal ranges of one component of `values` (-1: each value's length),
    with the lowest and highest, and how many weren't finite."""
    if numpy is not None:
        data = numpy.asarray(values, dtype=float)
        data = data.reshape(len(data), -1) if data.ndim > 1 else data.reshape(-1, 1)
        column = numpy.linalg.norm(data, axis=1) if component < 0 else data[:, min(component, data.shape[1] - 1)]
        finite = column[numpy.isfinite(column)]
        if finite.size == 0:
            return [0] * bins, 0.0, 0.0, int(column.size)
        low, high = float(finite.min()), float(finite.max())
        counts, _ = numpy.histogram(finite, bins=bins, range=(low, high if high > low else low + 1))
        return [int(n) for n in counts], low, high, int(column.size - finite.size)
    numbers = []
    skipped = 0
    for value in values:
        parts = list(value) if hasattr(value, "__len__") else [value]
        number = math.sqrt(sum(float(p) * float(p) for p in parts)) if component < 0 else float(parts[min(component, len(parts) - 1)])
        if math.isfinite(number):
            numbers.append(number)
        else:
            skipped += 1
    if not numbers:
        return [0] * bins, 0.0, 0.0, skipped
    low, high = min(numbers), max(numbers)
    width = (high - low) / bins if high > low else 1.0
    counts = [0] * bins
    for number in numbers:
        counts[min(int((number - low) / width), bins - 1)] += 1
    return counts, low, high, skipped


class Bars(QtWidgets.QWidget):
    """The histogram itself."""

    def __init__(self) -> None:
        super().__init__()
        self.counts: Sequence[int] = []
        self.setMinimumHeight(80)

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # noqa: N802 (Qt's name)
        painter = QtGui.QPainter(self)
        rect = self.rect().adjusted(6, 6, -6, -6)
        painter.fillRect(self.rect(), self.palette().base())
        top = max(self.counts, default=0)
        if not self.counts or top == 0:
            return
        width = rect.width() / len(self.counts)
        color = self.palette().highlight().color()
        for i, count in enumerate(self.counts):
            height = rect.height() * count / top
            painter.fillRect(QtCore.QRectF(rect.left() + i * width + 1, rect.bottom() - height, max(width - 2, 1), height), color)


class HistogramView(usdcc.ui.PrimvarView):
    """A histogram of the primvar in focus."""

    def __init__(self) -> None:
        self.component = 0
        self.bins = 32
        self.counts: list[int] = []

    def create_widget(self) -> QtWidgets.QWidget:
        widget = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(widget)
        options = QtWidgets.QHBoxLayout()
        self.component_box = QtWidgets.QComboBox()
        self.component_box.currentIndexChanged.connect(self._component_picked)
        self.bins_box = QtWidgets.QComboBox()
        for bins in BINS:
            self.bins_box.addItem(f"{bins} bins", bins)
        self.bins_box.setCurrentIndex(BINS.index(self.bins))
        self.bins_box.currentIndexChanged.connect(self._bins_picked)
        options.addWidget(self.component_box)
        options.addWidget(self.bins_box)
        options.addStretch(1)
        self.summary = QtWidgets.QLabel()
        self.summary.setObjectName("histogramSummary")
        self.bars = Bars()
        self.bars.setObjectName("histogramBars")
        layout.addLayout(options)
        layout.addWidget(self.bars, 1)
        layout.addWidget(self.summary)
        return widget

    def context_changed(self, context: usdcc.primvars.ViewContext, focus_only: bool) -> None:
        self._update()

    def _component_picked(self, index: int) -> None:
        if index >= 0 and self.component_box.itemData(index) is not None:
            self.component = self.component_box.itemData(index)
            self._update(keep_components=True)

    def _bins_picked(self, index: int) -> None:
        self.bins = self.bins_box.itemData(index)
        self._update(keep_components=True)

    def _update(self, keep_components: bool = False) -> None:
        context = self.context
        values = context.focused_values() if context is not None else None
        if values is None or not has_numbers(context):
            self.counts = []
            self.bars.counts = []
            self.summary.setText("No numbers to count.")
            self.bars.update()
            return
        values = values if hasattr(values, "__len__") and not isinstance(values, str) else [values]
        first = values[0] if len(values) else 0
        size = len(first) if hasattr(first, "__len__") else 1
        if not keep_components:
            self.component_box.blockSignals(True)
            self.component_box.clear()
            if size == 1:
                self.component_box.addItem("Value", 0)
            else:
                for i in range(size):
                    self.component_box.addItem("xyzw"[i] if i < 4 else str(i), i)
                self.component_box.addItem("Length", -1)
            index = self.component_box.findData(self.component)
            self.component_box.setCurrentIndex(max(index, 0))
            self.component = self.component_box.currentData()
            self.component_box.blockSignals(False)
        self.counts, low, high, skipped = histogram(values, self.component, self.bins)
        self.bars.counts = self.counts
        self.bars.update()
        text = f"{len(values)} values from {low:.4g} to {high:.4g}"
        if skipped:
            text += f", {skipped} not finite"
        self.summary.setText(text)

    def save_state(self) -> dict[str, int]:
        return {"component": self.component, "bins": self.bins}

    def restore_state(self, state: Any) -> None:
        if not isinstance(state, dict):
            return
        self.component = int(state.get("component", 0))
        if state.get("bins") in BINS:
            self.bins = state["bins"]
            self.bins_box.setCurrentIndex(BINS.index(self.bins))


def activate(plugin: Any) -> None:
    usdcc.primvars.register_view(plugin.id + VIEW_SUFFIX, "Histogram", HistogramView, applies=has_numbers)
