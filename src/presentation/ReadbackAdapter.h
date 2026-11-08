#pragma once
#include "PresentationAdapter.h"
#include <QImage>
#include <memory>
namespace demo {
// Boundary controls schedule completion only; production readback has no GPU work.
// Tests may hold these operations, not replace lifecycle/admission policy.
using PresentationBoundary = std::function<void(std::string, Done)>;
class ReadbackAdapter : public PresentationAdapter {
public:
 explicit ReadbackAdapter(PresentationEvents, PresentationBoundary = {});
 ~ReadbackAdapter() override;
 void probe(tc::DeviceInfo const&, std::function<void(Result<ProbeReport>)>) override;
 void preparePath(PresentationPath, std::shared_ptr<NativeHostLease>, Done) override;
 TargetDesc describeTarget(ViewportExtent const&) const override;
 void consumeImage(OwnedFrame) override;
 void beginDrain(Done) override;
 void releasePath(Done) override;
 // Session binds the actual new epoch + target extent before accepting images.
 // Updating acceptance never resizes/creates targets or changes renderer backend.
 void setFrameContext(SessionGeneration, ViewportExtent const&) override;
 static QImage copyImage(lr::render::ImageRgba8 const&);
private:
 struct Core;
 std::shared_ptr<Core> core_;
};
}
