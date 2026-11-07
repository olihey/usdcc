"""The plugin's settings (Edit > Settings... > RTX, docs/RTX_RENDER.md section 5)."""

import usdcc.settings

from . import scene

CATEGORY = "RTX"
PYTHON = "RtxRender/python"
MODE = "RtxRender/mode"
MAX_FRAMES = "RtxRender/maxFrames"
IMAGE_MS = "RtxRender/imageInterval"
EDIT_DELAY_MS = "RtxRender/editDelay"

MODES = list(scene.MODES)

# Tests set this: the helper then renders a gradient instead of importing ovrtx.
FAKE_RENDERER = False


def define():
    usdcc.settings.define(PYTHON, "string", "", label="Other Python", category=CATEGORY, group="ovrtx",
                          description="Empty: usdcc's own Python, with the ovrtx the plugin's packages installed. "
                                      "Or a python.exe of your own with ovrtx, ovstage and numpy installed "
                                      "(Python 3.11 to 3.13), to try another ovrtx version.")
    usdcc.settings.define(MODE, "choice", MODES[0], label="Mode", category=CATEGORY, group="Rendering",
                          description="What a new RTX Render panel renders with: real-time path tracing, "
                                      "path tracing, or minimal rasterizing.", choices=MODES)
    usdcc.settings.define(MAX_FRAMES, "int", 128, label="Frames after a change", category=CATEGORY, group="Rendering",
                          description="How many frames are rendered after a change before the image is done. "
                                      "In Path Tracing, the final samples per pixel, with progressive previews.",
                          minimum=1, maximum=4096)
    usdcc.settings.define(IMAGE_MS, "int", 33, label="Image updates (ms)", category=CATEGORY, group="Rendering",
                          description="How often a new image is sent to the panel at most.", minimum=10, maximum=2000)
    usdcc.settings.define(EDIT_DELAY_MS, "int", 30, label="Update after edits (ms)", category=CATEGORY,
                          group="Rendering", description="How long after the last edit it reaches the renderer.",
                          minimum=0, maximum=10000)


def get(key):
    return usdcc.settings.get(key)
