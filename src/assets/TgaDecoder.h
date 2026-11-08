#pragma once
#include "CpuAsset.h"
namespace demo {
// Raw/RLE truecolor24/32 and gray8 only. Canonical top-left RGBA8, no UV flip.
// Stored bytes unchanged except channel/origin conversion; encoding is metadata.
// Caps:128MiB input,64MiB RGBA. Color maps/interleaving unsupported.

Result<lr::core::ImageData> decodeTga(std::vector<std::uint8_t> const &, lr::core::ColorEncoding);
Result<lr::core::ImageData> decodeTga(std::vector<std::uint8_t> const &, lr::core::ColorEncoding,
                                      std::function<bool()> const &stopRequested);
} // namespace demo
