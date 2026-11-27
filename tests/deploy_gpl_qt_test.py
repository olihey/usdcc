"""tools/deploy.py's check that the standard (BSD-3-Clause) release has no Qt module that
is GPL-3.0 only (docs/RELEASE.md section 6), on each platform's library names."""

import argparse
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "ci"))
import deploy

STANDARD = argparse.Namespace(with_blender_mesh_tools=False)
GPL = argparse.Namespace(with_blender_mesh_tools=True)


def touch(root, name):
    path = Path(root) / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")


class GplOnlyQtTest(unittest.TestCase):
    def setUp(self):
        self._folder = tempfile.TemporaryDirectory()
        self.root = Path(self._folder.name)
        # What a standard deploy has: LGPL Qt, the licence files named after modules.
        for name in ("bin/Qt6Core.dll", "bin/Qt6Network.dll", "bin/Qt6WebSockets.dll", "lib/libQt6Core.so.6",
                     "lib/libQt6WebSockets.so.6.11.1", "Resources/lib/libQt6Network.6.dylib",
                     "python/Lib/site-packages/PySide6/QtCore.pyd", "lib/python3.12/site-packages/PySide6/QtWidgets.abi3.so",
                     "licenses/vcpkg/qthttpserver.txt", "bin/QtNodes.dll"):
            touch(self.root, name)

    def tearDown(self):
        self._folder.cleanup()

    def test_standard_deploy_passes(self):
        self.assertEqual(deploy.gpl_only_qt_libraries(self.root), [])
        deploy.check_no_gpl_only_qt(self.root, STANDARD)

    def test_each_platform_name_fails(self):
        for name in ("bin/Qt6HttpServer.dll", "lib/libQt6HttpServer.so.6", "Resources/lib/libQt6HttpServer.6.dylib",
                     "Frameworks/QtCharts.framework", "bin/Qt6ChartsQml.dll", "bin/Qt6DataVisualization.dll",
                     "lib/libQt6Graphs.so.6.11.1", "bin/Qt6VirtualKeyboard.dll", "bin/Qt6Quick3DPhysics.dll",
                     "python/Lib/site-packages/PySide6/QtHttpServer.pyd",
                     "lib/python3.12/site-packages/PySide6/QtCharts.abi3.so"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                touch(folder, "bin/Qt6Core.dll")
                touch(folder, name)
                self.assertEqual(deploy.gpl_only_qt_libraries(folder), [name])
                with self.assertRaises(SystemExit) as raised:
                    deploy.check_no_gpl_only_qt(folder, STANDARD)
                self.assertIn(name, str(raised.exception))

    def test_gpl_configuration_may_have_one(self):
        touch(self.root, "bin/Qt6HttpServer.dll")
        deploy.check_no_gpl_only_qt(self.root, GPL)


if __name__ == "__main__":
    unittest.main()
