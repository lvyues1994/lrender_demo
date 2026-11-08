#pragma once
#include "AssetCatalog.h"
namespace demo {
// Fixed catalog only, root-contained canonical paths;128MiB aggregate CpuAsset cap.
// No World/Qt/GPU access; read chunks and parse loops check cooperative stop.

// Blocking CPU-only. Cooperative cancellation is a Load error with cause="cancelled";
// P8 maps its latched stop state to LoadTicket Cancelled, not a partially loaded asset.
Result<CpuAssetPtr> decodeAsset(AssetEntry const &, std::function<bool()> stopRequested);
} // namespace demo
