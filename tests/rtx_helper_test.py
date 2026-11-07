"""Exercise the real RTX scheduling path without loading ovrtx or requiring a GPU."""

import importlib.util
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

HELPER = (
    Path(__file__).resolve().parents[1]
    / "src/plugins/rtx_render/python/usdcc_rtx_render/helper/rtx_helper.py"
)
spec = importlib.util.spec_from_file_location("rtx_helper", HELPER)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def backend(samples, path_tracing=True):
    writer = Mock()
    result = helper.RtxBackend(writer)
    result.loaded = True
    result.max_frames = samples
    result.path_tracing = path_tracing
    result.image_seconds = 0
    pixels = helper._Pixels(bytes((20, 40, 60, 255)), 1, 1)
    mapped = Mock()
    mapped.copy.return_value = pixels
    result.np = SimpleNamespace(
        uint32="uint32",
        array=lambda values, dtype: values,
        from_dlpack=lambda var: mapped,
    )
    result.ovrtx = SimpleNamespace(Device=SimpleNamespace(CPU="cpu"))
    frame = SimpleNamespace(render_vars={helper.LDR_COLOR: Mock()})
    product = SimpleNamespace(frames=[frame])
    result.renderer = Mock()
    result.renderer.step.return_value = {helper.PRODUCT: product}
    result._write = Mock()
    result._commit = Mock()
    result.cmd_start()
    return result, writer


def finish(result):
    for _ in range(256):
        if not result.wants_frames():
            return
        result.render_frame()
    raise AssertionError("render did not finish")


class PathTracingTest(unittest.TestCase):
    def test_preview_then_bounded_refinements(self):
        result, writer = backend(128)
        result.render_frame()
        writer.image.assert_called_once_with(1, 1, bytes((20, 40, 60, 255)), 1)
        self.assertTrue(result.wants_frames())
        finish(result)
        budgets = [call.args[2][0] for call in result._write.call_args_list]
        self.assertEqual(budgets, [1, 2, 4, 8, 16, 32, 64, 128])
        self.assertEqual(writer.image.call_count, len(budgets))
        writer.event.assert_called_with("state", state="done")
        ordinals = [
            call.kwargs["ordinal"] for call in result.renderer.step.call_args_list
        ]
        self.assertEqual(ordinals, sorted(set(ordinals)))

    def test_exact_budget_for_one_and_non_power_of_two(self):
        for samples, expected in (
            (1, [1]),
            (3, [1, 2, 3]),
            (129, [1, 2, 4, 8, 16, 32, 64, 128, 129]),
        ):
            with self.subTest(samples=samples):
                result, writer = backend(samples)
                finish(result)
                self.assertEqual(
                    [call.args[2][0] for call in result._write.call_args_list], expected
                )
                writer.event.assert_called_with("state", state="done")

    def test_stop_and_changes_allow_another_preview(self):
        result, writer = backend(128)
        result.render_frame()
        result.cmd_stop()
        self.assertFalse(result.wants_frames())
        result.cmd_start()
        finish(result)
        result.changed()  # camera, size, or scene edit after completion
        self.assertTrue(result.wants_frames())
        result.render_frame()
        self.assertEqual(result._write.call_args.args[2], [1])
        writer.event.assert_called_with("state", state="rendering")

    def test_real_time_keeps_its_frame_budget(self):
        result, writer = backend(3, path_tracing=False)
        finish(result)
        self.assertEqual(result.renderer.step.call_count, 3)
        result._write.assert_not_called()
        self.assertEqual(writer.image.call_count, 3)
        writer.event.assert_called_with("state", state="done")


if __name__ == "__main__":
    unittest.main()
