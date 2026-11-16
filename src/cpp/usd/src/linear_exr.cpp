#include "usdcc/usd/linear_exr.h"

#ifdef USDCC_HAS_OPENEXR
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#endif

#include <exception>

namespace usdcc::usd {

bool linearExrAvailable() {
#ifdef USDCC_HAS_OPENEXR
    return true;
#else
    return false;
#endif
}

bool writeLinearExr(const std::string& path, const float* rgba, int width, int height, std::string* error,
                    size_t rowFloats) {
    const auto fail = [error](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (!rgba || width < 1 || height < 1) return fail("no pixels");
#ifdef USDCC_HAS_OPENEXR
    if (rowFloats == 0) rowFloats = size_t(width) * 4;
    try {
        Imf::Header header(width, height);
        header.compression() = Imf::ZIP_COMPRESSION;
        // The working space: linear Rec.709 primaries, D65 (OpenEXR's default chromaticities).
        Imf::addChromaticities(header, Imf::Chromaticities());
        Imf::FrameBuffer frame;
        const char* names[] = {"R", "G", "B", "A"};
        for (int c = 0; c < 4; ++c) {
            header.channels().insert(names[c], Imf::Channel(Imf::FLOAT));
            frame.insert(names[c], Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(const_cast<float*>(rgba + c)),
                                              sizeof(float) * 4, sizeof(float) * rowFloats));
        }
        Imf::OutputFile file(path.c_str(), header);
        file.setFrameBuffer(frame);
        file.writePixels(height);
        return true;
    } catch (const std::exception& exception) {
        return fail(exception.what());
    }
#else
    (void)path;
    (void)rowFloats;
    return fail("usdcc was built without OpenEXR");
#endif
}

}  // namespace usdcc::usd
