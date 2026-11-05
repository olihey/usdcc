"""The plugin's settings (Edit > Settings... > V-Ray, docs/VRAY_RENDER.md section 6)."""

import os

import usdcc.settings

CATEGORY = "V-Ray"
SDK = "VRayRender/appSdkFolder"
PYTHON = "VRayRender/python"
LICENSE_SERVERS = "VRayRender/licenseServers"
ENGINE = "VRayRender/engine"
UPDATE_MS = "VRayRender/updateInterval"
EDIT_DELAY_MS = "VRayRender/editDelay"

ENGINES = ["CPU", "CUDA", "RTX"]

# Tests set this: the helper then renders a gradient instead of loading V-Ray.
FAKE_RENDERER = False


def default_sdk_folder():
    """VRAY_APPSDK, the folder above VRAY_APPSDK_BIN, or the installer's default
    folder if it exists; else empty."""
    if os.environ.get("VRAY_APPSDK"):
        return os.environ["VRAY_APPSDK"]
    if os.environ.get("VRAY_APPSDK_BIN"):
        return os.path.dirname(os.path.normpath(os.environ["VRAY_APPSDK_BIN"]))
    installed = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Chaos", "V-Ray", "AppSDK")
    return installed if os.path.isdir(installed) else ""


def define():
    usdcc.settings.define(SDK, "folder", default_sdk_folder(), label="App SDK folder", category=CATEGORY,
                          group="V-Ray", description="The V-Ray App SDK's folder, the one with bin and python in it.")
    usdcc.settings.define(PYTHON, "string", "", label="Python", category=CATEGORY, group="V-Ray",
                          description="The python.exe V-Ray runs in, which must be a version the App SDK supports. "
                                      "Empty: usdcc's own.")
    usdcc.settings.define(LICENSE_SERVERS, "string", "", label="License server", category=CATEGORY, group="V-Ray",
                          description="host or host:port. Empty: V-Ray's own license setup (vrlclient.xml).")
    usdcc.settings.define(ENGINE, "choice", "CPU", label="Engine", category=CATEGORY, group="Rendering",
                          description="What a new V-Ray Render panel renders with.", choices=ENGINES)
    usdcc.settings.define(UPDATE_MS, "int", 200, label="Image updates (ms)", category=CATEGORY, group="Rendering",
                          description="How often V-Ray sends a refined image at most.", minimum=30, maximum=5000)
    usdcc.settings.define(EDIT_DELAY_MS, "int", 500, label="Re-render after edits (ms)", category=CATEGORY,
                          group="Rendering", description="How long after the last edit the stage is sent to V-Ray again.",
                          minimum=50, maximum=10000)


def get(key):
    return usdcc.settings.get(key)
