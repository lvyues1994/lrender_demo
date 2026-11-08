#pragma once
#include "render/SessionTypes.h"
namespace demo {
struct NativeHostLease;
// GUI controls/callbacks; never wait. Session alone owns World/targets. Submitted
// work is irrevocable: settle ALL completions before beginDrain, destroy target
// before releasePath. Adapter owns only presentation resources, never scene data.
class PresentationAdapter {
public:
 virtual ~PresentationAdapter() = default;
 virtual void probe(tc::DeviceInfo const&, std::function<void(Result<ProbeReport>)>) = 0;
 virtual void preparePath(PresentationPath, std::shared_ptr<NativeHostLease>, Done) = 0;
 virtual TargetDesc describeTarget(ViewportExtent const&) const = 0;
 // The original six signatures carry no generation. This one narrow context
 // binding lets the type-erased Session configure admission without downcasting.
 // Call only after target readiness; it neither creates nor resizes that target.
 virtual void setFrameContext(SessionGeneration, ViewportExtent const&) = 0;
 virtual void consumeImage(OwnedFrame) = 0;
 virtual void beginDrain(Done) = 0;
 virtual void releasePath(Done) = 0;
};
enum class TransitionAction { Idle, AwaitOldFrames, DrainOld, DestroyOldTarget, ReleaseOld, PrepareNew, PublishNew, Closed };
// Value-only handoff protocol; does NOT operate on a renderer or pretend targets
// have been destroyed. Session must acknowledge each real operation, in order.
class PresentationTransition {
public:
 void fallback(RenderError);
 void close();
 void completed(TransitionAction);
 TransitionAction action() const { return action_; }
 std::optional<RenderError> const& reason() const { return reason_; }
private:
 TransitionAction action_ = TransitionAction::Idle;
 std::optional<RenderError> reason_;
 bool closing_ = false;
};
}
