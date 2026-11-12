"""Run by tools/ci/rtx_smoke_test.py inside the packaged application."""

import builtins
import ctypes
import json
import os
import sys
import time
import traceback
from pathlib import Path

import pxr
import PySide6
import usdcc
import usdcc.app
import usdcc.settings
import usdcc.usd
from PySide6 import QtCore, QtGui, QtWidgets

root = Path(os.environ["USDCC_RTX_SMOKE_ROOT"]).resolve()
work = Path(os.environ["USDCC_RTX_SMOKE_WORK"])
mode = os.environ["USDCC_RTX_SMOKE_MODE"]
QtCore.QCoreApplication.setAttribute(QtCore.Qt.AA_DontUseNativeDialogs, True)
start = time.monotonic()
panel = None
phase = "start"
serial = 0
stage = usdcc.usd.current_stage()
source = stage.Flatten().ExportToString()
result = {"passed": False, "mode": mode}
result["profile"] = QtCore.QCoreApplication.applicationName()
result["profile_data"] = QtCore.QStandardPaths.writableLocation(
    QtCore.QStandardPaths.AppLocalDataLocation
)


def module_path(name):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    kernel.GetModuleFileNameW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.c_uint,
    ]
    handle = kernel.GetModuleHandleW(name)
    if not handle:
        return None
    buffer = ctypes.create_unicode_buffer(32768)
    assert kernel.GetModuleFileNameW(handle, buffer, len(buffer)), name
    return Path(buffer.value).resolve()


def inside(path):
    return path is not None and path.is_relative_to(root)


def outline_pixel(color):
    return color.red() > 210 and 80 < color.green() < 190 and color.blue() < 60


def finish(error=None):
    timer.stop()
    result.update(passed=error is None, seconds=round(time.monotonic() - start, 2))
    if error:
        result["error"] = error
    (work / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    usdcc.app.main_window().close()  # exercises the native SDK/GPU drain


def tick():
    global panel, phase, serial
    timer.stop()  # QFileDialog.exec runs a nested loop
    try:
        assert time.monotonic() - start < 600, (
            "No completed frame/save within ten minutes"
        )
        if phase == "start":
            assert result["profile"].startswith("usdcc-test-rtx-"), (
                "Test profile was not selected before startup"
            )
            assert not any(
                n.split(".")[0] in ("ovrtx", "ovstage", "usdcc_rtx_render")
                for n in sys.modules
            )
            # The runner starts hidden like other background helpers. GPU
            # presentation requires an exposed window, so show the test surface.
            main = usdcc.app.main_window()
            main.hide()
            main.show()
            for package in (usdcc, pxr, PySide6):
                assert inside(Path(package.__file__).resolve()), package.__file__
            available = dict(usdcc.app.view_panel_types())
            if mode == "Omitted":
                assert "org.usdcc.rtx-render.view" not in available, (
                    "RTX unexpectedly shipped"
                )
                assert module_path("rtx_render_native.dll") is None
                finish()
                return
            assert "org.usdcc.rtx-render.view" in available, (
                "Native RTX did not activate"
            )
            assert inside(module_path("rtx_render_native.dll")), (
                "Native plugin came from outside deploy"
            )
            assert module_path("ovrtx-dynamic.dll") is None, (
                "SDK eagerly loaded before Render"
            )
            usdcc.settings.set("RtxRender/sampleBudget", 2)
            usdcc.usd.select(["/World/Sphere"], stage)
            previous = {
                w
                for w in QtWidgets.QApplication.allWidgets()
                if w.property("nativeRtxImageSize") is not None
            }
            usdcc.app.open_view_panel("org.usdcc.rtx-render.view", stage)
            panel = next(
                w
                for w in QtWidgets.QApplication.allWidgets()
                if w.property("nativeRtxImageSize") is not None and w not in previous
            )
            panel.window().resize(1200, 800)
            panel.findChild(QtWidgets.QComboBox, "nativeRtxMode").setCurrentText(
                "Path Tracing"
            )
            cameras = panel.findChild(QtWidgets.QComboBox, "renderCamera")
            cameras.setCurrentIndex(cameras.findData("/Render/Camera"))
            panel.findChild(QtWidgets.QComboBox, "renderResolution").setCurrentIndex(3)
            panel.findChild(QtWidgets.QToolButton, "renderButton").click()
            phase = "frame"
        elif phase == "frame":
            status = panel.findChild(QtWidgets.QLabel, "renderStatus").text()
            result["last_status"] = status
            result["exposed"] = [
                (w.objectName(), w.isExposed())
                for w in QtGui.QGuiApplication.allWindows()
                if w.objectName() == "nativeRtxWindow"
            ]
            (work / "progress.json").write_text(
                json.dumps(result, indent=2), encoding="utf-8"
            )
            if mode == "MissingRuntime":
                messages = [w.text() for w in panel.findChildren(QtWidgets.QLabel)]
                assert any("Native RTX runtime missing" in text for text in messages), (
                    messages
                )
                assert not panel.findChild(
                    QtWidgets.QToolButton, "renderButton"
                ).isChecked()
                assert module_path("ovrtx-dynamic.dll") is None
                finish()
                return
            size = panel.property("nativeRtxImageSize")
            if size.isValid() and status.startswith("Done"):
                result["size"] = [size.width(), size.height()]
                result["libraries"] = {}
                for name in ("ovrtx-dynamic.dll", "ovstage-dynamic.dll"):
                    path = module_path(name)
                    assert inside(path), f"{name} loaded outside deploy: {path}"
                    result["libraries"][name] = str(path)
                panel.findChild(QtWidgets.QToolButton, "renderButton").click()
                serial = int(panel.property("nativeRtxFrameSerial"))
                QtCore.QTimer.singleShot(100, accept_save)
                panel.findChild(QtWidgets.QToolButton, "renderSave").click()
                phase = "save"
        elif phase == "save":
            filename = work / "clean.png"
            if filename.exists():
                image = QtGui.QImage(str(filename))
                assert not image.isNull()
                # The fixture's cube is itself orange. Inspect the selected blue
                # sphere on the right, excluding that authored material color.
                orange = sum(
                    1
                    for y in range(image.height())
                    for x in range(image.width() * 3 // 5, image.width())
                    if outline_pixel(image.pixelColor(x, y))
                )
                assert orange == 0, f"Saved selection outline: {orange} orange pixels"
                assert int(panel.property("nativeRtxFrameSerial")) == serial, (
                    "Save submitted a render while stopped"
                )
                assert stage.Flatten().ExportToString() == source, (
                    "Render modified the source"
                )
                assert not any(
                    n.split(".")[0] in ("ovrtx", "ovstage", "usdcc_rtx_render")
                    for n in sys.modules
                ), "Python RTX was imported"
                result["clean_pixels"] = image.width() * image.height()
                finish()
                return
    except Exception:  # noqa: BLE001 -- report failures from Qt callbacks to the runner
        finish(traceback.format_exc())
        return
    timer.start(100)


def accept_save():
    try:
        dialog = next(
            w
            for w in QtWidgets.QApplication.topLevelWidgets()
            if isinstance(w, QtWidgets.QFileDialog) and w.isVisible()
        )
        dialog.setDirectory(str(work))
        dialog.findChild(QtWidgets.QLineEdit, "fileNameEdit").setText("clean.png")
        dialog.accept()
    except Exception:  # noqa: BLE001 -- report failures from the nested dialog loop
        finish(traceback.format_exc())


timer = QtCore.QTimer()
timer.timeout.connect(tick)
builtins.usdcc_rtx_smoke_timer = timer
timer.start(300)
