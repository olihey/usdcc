"""Executed inside the packaged app by tools/ci/blender_mesh_smoke_test.py."""

import builtins
import ctypes
import json
import os
import sys
import traceback
from pathlib import Path

root = Path(os.environ["USDCC_BLENDER_SMOKE_ROOT"]).resolve()
work = Path(os.environ["USDCC_BLENDER_SMOKE_WORK"])
omitted = os.environ.get("USDCC_BLENDER_SMOKE_OMITTED") == "1"
windows = sys.platform == "win32"
result = {"passed": False, "omitted": omitted}
try:
    plugin_root = root / "plugins/blender_mesh_tools"
    manifest_file = plugin_root / "usdcc-plugin.json"
    platform = json.loads(manifest_file.read_text())["native"]["built_for"]["platform"] if manifest_file.is_file() else "omitted"
    backend = plugin_root / "bin" / platform / ("usdcc_blender_mesh_backend.dll" if windows else "libusdcc_blender_mesh_backend.so")
    if windows:
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
        kernel.GetModuleHandleW.restype = ctypes.c_void_p
        kernel.GetModuleFileNameW.argtypes = [
            ctypes.c_void_p,
            ctypes.c_wchar_p,
            ctypes.c_uint,
        ]
        handle = kernel.GetModuleHandleW("usdcc_blender_mesh_backend.dll")
        buffer = ctypes.create_unicode_buffer(32768)
        if handle:
            assert kernel.GetModuleFileNameW(handle, buffer, len(buffer))
        loaded = buffer.value
    else:
        maps = Path("/proc/self/maps").read_text()
        loaded = str(backend.resolve()) if str(backend.resolve()) in maps else ""
    if omitted:
        assert not loaded and not backend.exists(), (
            "Standard artifact loaded Blender code"
        )
    else:
        assert loaded and Path(loaded).resolve() == backend.resolve(), (
            f"Backend was not activated from this package: {loaded}"
        )
        result["backend_module"] = loaded
        lib = ctypes.CDLL(str(backend))
        lib.usdcc_blender_mesh_probe.argtypes = [
            ctypes.c_uint,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        report = ctypes.create_string_buffer(1024)
        assert lib.usdcc_blender_mesh_probe(100, report, len(report)) == 0, report.value
        result["probe"] = json.loads(report.value)
    try:
        from PySide6 import QtCore, QtGui, QtWidgets
    except ImportError:
        if not omitted:
            lib.usdcc_blender_mesh_shutdown()
        result["passed"] = True
        result["menu_test"] = (
            "C++ integration tests cover menus; this Linux package has no PySide6"
        )
        (work / "result.json").write_text(json.dumps(result, indent=2))
        os._exit(0)
    import usdcc.app
    import usdcc.usd
    from pxr import UsdGeom, UsdUtils

    main = usdcc.app.main_window()
    stage = usdcc.usd.current_stage()
    cache_id = UsdUtils.StageCache.Get().GetId(stage).ToLongInt()
    source = stage.GetRootLayer().ExportToString()
    phase = 0

    def done(error=None):
        if error:
            result["error"] = str(error)
        result["passed"] = error is None
        (work / "result.json").write_text(json.dumps(result, indent=2))
        if error:
            QtCore.QCoreApplication.exit(1)
        else:
            QtCore.QTimer.singleShot(0, QtWidgets.QApplication.quit)
            main.close()  # successful edit is undone before close; exercises shutdown

    def tick():
        global phase
        try:
            if omitted:
                done()
                return
            jobs = main.property("BlenderMeshJobs")
            assert jobs, "Mesh job controller absent"
            if phase == 0:
                usdcc.app.stage_manager().set_selected_paths(cache_id, ["/Mesh"])
                action = next(
                    a
                    for a in main.findChildren(QtGui.QAction)
                    if a.text().startswith("Triangulate")
                )

                def accept_options():
                    dialog = QtWidgets.QApplication.activeModalWidget()
                    assert dialog and dialog.objectName() == "BlenderMeshOptions"
                    dialog.accept()

                QtCore.QTimer.singleShot(0, accept_options)
                action.trigger()
                phase = 1
            elif not jobs.property("busy"):
                assert list(
                    UsdGeom.Mesh(stage.GetPrimAtPath("/Mesh"))
                    .GetFaceVertexCountsAttr()
                    .Get()
                ) == [3, 3], jobs.property("status")
                undo = usdcc.app.undo_manager()
                after = stage.GetRootLayer().ExportToString()
                undo.undo(cache_id)
                assert stage.GetRootLayer().ExportToString() == source
                undo.redo(cache_id)
                assert stage.GetRootLayer().ExportToString() == after
                undo.undo(cache_id)
                result["menu_test"] = "triangulate, undo and redo passed"
                timer.stop()
                done()
        except Exception:  # noqa: BLE001 - record any packaged app failure before quitting
            timer.stop()
            done(traceback.format_exc())

    timer = QtCore.QTimer()
    timer.timeout.connect(tick)
    builtins.usdcc_blender_smoke_timer = timer
    timer.start(50)
except Exception:  # noqa: BLE001 - the smoke harness needs a diagnostic for every failure
    result["error"] = traceback.format_exc()
    (work / "result.json").write_text(json.dumps(result, indent=2))
    os._exit(1)
