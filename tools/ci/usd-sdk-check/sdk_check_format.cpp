// A minimal OpenUSD file format plugin, built by the SDK check (CMakeLists.txt).
// A .usdccsdkcheck file holds one prim name; reading it gives a layer with an
// Xform prim of that name.
#include <pxr/pxr.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>

#include <fstream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

class UsdccSdkCheckFileFormat : public SdfFileFormat
{
public:
    bool CanRead(const std::string& path) const override
    {
        return TfStringEndsWith(path, ".usdccsdkcheck");
    }

    bool Read(SdfLayer* layer, const std::string& resolvedPath, bool /*metadataOnly*/) const override
    {
        std::ifstream in(resolvedPath);
        std::string name;
        std::getline(in, name);
        name = TfStringTrim(name);
        if (!TfIsValidIdentifier(name)) {
            TF_RUNTIME_ERROR("'%s' doesn't start with a valid prim name", resolvedPath.c_str());
            return false;
        }
        SdfLayerRefPtr content = SdfLayer::CreateAnonymous(".usda");
        SdfPrimSpec::New(content->GetPseudoRoot(), name, SdfSpecifierDef, "Xform");
        layer->TransferContent(content);
        return true;
    }

protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;

    UsdccSdkCheckFileFormat()
        : SdfFileFormat(TfToken("usdccsdkcheck"), TfToken("1.0"), TfToken("usd"), "usdccsdkcheck")
    {
    }
    ~UsdccSdkCheckFileFormat() override = default;
};

TF_REGISTRY_FUNCTION(TfType)
{
    SDF_DEFINE_FILE_FORMAT(UsdccSdkCheckFileFormat, SdfFileFormat);
}
