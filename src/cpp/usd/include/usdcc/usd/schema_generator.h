#pragma once

#include "usdcc/core/export.h"

#include "usdcc/usd/schema_library.h"

#include <filesystem>
#include <string>
#include <vector>

namespace usdcc::usd {

// How to run OpenUSD's Python as a process: the app's interpreter and its
// sys.path (PythonRuntime::executable() and settings().paths), where pxr and
// the usdcc package, with usdGenSchema's jinja2 in usdcc/_vendor, are found.
struct SchemaPython {
    std::filesystem::path executable;
    std::vector<std::filesystem::path> paths;
    // Put ahead on PATH: where OpenUSD's libraries are (a deploy's bin).
    std::vector<std::filesystem::path> dllFolders;
};

struct SchemaGenerateResult {
    bool ok = false;
    std::string error;                // why not, for the user
    std::vector<SchemaIssue> issues;  // checkSchemaLibrary()'s, warnings included
    std::string output;               // usdGenSchema's, without its boilerplate
    std::vector<std::string> tried;   // the schemas the trial made a prim of, or applied
};

// Generates the codeless library `library` in `usdFolder` (docs/SCHEMAS.md
// §5.3): reads usd/<library>/schema.usda and checks it; runs usdGenSchema in a
// process of its own, into a temporary folder, with `baseLibraries` (the usd
// folders of the libraries it builds on) on PXR_PLUGINPATH_NAME; fills in
// plugInfo.json's placeholders; tries the result in another process (each
// prim type made, each API applied); and only then puts plugInfo.json and
// generatedSchema.usda in place. Anything that fails leaves the folder as it
// was. Blocks: up to `timeoutMs` per process. The app sees the new schemas at
// its next start (§2, trap 1).
USDCC_USD_UI_API SchemaGenerateResult generateSchemaLibrary(const std::string& usdFolder, const std::string& library,
                                                            const SchemaPython& python,
                                                            const std::vector<std::string>& baseLibraries = {},
                                                            int timeoutMs = 120000);

}  // namespace usdcc::usd
