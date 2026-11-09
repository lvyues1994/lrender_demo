#pragma once

#include "render/workflows/AdapterSenders.h"

#include <lqtexec/ObjectScope.h>

#include <lexec/execution.hpp>

#include <utility>

namespace demo::workflows {

// Soft rebuild (VulkanShared): drain only — keep presenter/host.
// Compose: beginDrainSender | then_on(GUI, destroy+bindFresh). Never releasePath.
inline auto softDrainOnly(PresentationAdapter &adapter) { return beginDrainSender(adapter); }

// Hard path pieces for drain | then_on(destroy) | release | then_on(create/finish).
// Nested spawn is used when release must start after destroy on the GUI hop
// (let_value→any_sender erase is not portable across these Result completions).

template <class GuiFn>
auto drainThenOnGui(PresentationAdapter &adapter, lqtexec::ObjectScope &scope, GuiFn &&fn) {
  return beginDrainSender(adapter) | lqtexec::then_on(scope, static_cast<GuiFn &&>(fn));
}

template <class GuiFn>
auto releaseThenOnGui(PresentationAdapter &adapter, lqtexec::ObjectScope &scope, GuiFn &&fn) {
  return releasePathSender(adapter) | lqtexec::then_on(scope, static_cast<GuiFn &&>(fn));
}

} // namespace demo::workflows
