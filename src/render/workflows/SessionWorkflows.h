#pragma once

#include "render/workflows/AdapterSenders.h"

namespace demo::workflows {

// Soft rebuild (VulkanShared): drain only — keep presenter/host; caller hops GUI then
// destroyTarget/bindFreshTarget. Compose with then_on + upon_error before spawn
// (same pattern as SessionFrameSubmit).
inline auto softDrainOnly(PresentationAdapter &adapter) { return beginDrainSender(adapter); }

} // namespace demo::workflows
