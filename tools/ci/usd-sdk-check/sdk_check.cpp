// The SDK check (CMakeLists.txt): registers the file format plugin built next
// to this program, writes a .usdccsdkcheck file, opens it as a stage and checks
// that the prim the plugin makes is there.
#include <pxr/pxr.h>
#include <pxr/base/arch/systemInfo.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <cstdio>
#include <fstream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

int main()
{
    const std::string dir = TfGetPathName(ArchGetExecutablePath());
    PlugRegistry::GetInstance().RegisterPlugins(dir);

    const std::string path = TfAbsPath(dir + "check.usdccsdkcheck");
    std::ofstream(path) << "SdkCheck\n";

    UsdStageRefPtr stage = UsdStage::Open(path);
    if (!stage) {
        std::fprintf(stderr, "FAILED: couldn't open %s through the plugin\n", path.c_str());
        return 1;
    }
    UsdGeomXform xform(stage->GetPrimAtPath(SdfPath("/SdkCheck")));
    if (!xform) {
        std::fprintf(stderr, "FAILED: no Xform /SdkCheck in %s\n", path.c_str());
        return 1;
    }
    std::printf("OK: OpenUSD %d.%d loaded the plugin and read %s\n",
                PXR_MINOR_VERSION, PXR_PATCH_VERSION, path.c_str());
    return 0;
}
