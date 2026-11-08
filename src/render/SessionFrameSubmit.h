#pragma once
#include "SessionTypes.h"
#include <lexec/execution.hpp>
#include <lqtexec/ObjectScope.h>
namespace demo {
// Renderer already queued on return. scope/root MUST outlive producer settlement,
// independent of any UI ObjectScope. spawn retains the receiver operation.
void submitSessionFrame(lr::render::Renderer &, lr::render::FrameDesc, FrameStamp, lqtexec::ObjectScope &,
                        lexec::counting_scope &, std::function<void(FrameCompletion)>);
} // namespace demo
