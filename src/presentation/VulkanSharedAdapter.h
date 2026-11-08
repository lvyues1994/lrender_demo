#pragma once
#include "NativeSurfaceFactory.h"
#include "ReadbackAdapter.h"
#include <lqtexec/ObjectScope.h>
#include <lr/present/VulkanPresenter.h>
namespace demo {
// GUI control only; presenter init/flush/last release run on a fixed external
// worker. Application scope/scheduler and host provider outlive
// Session::onStopped.
class VulkanSharedAdapter final : public PresentationAdapter {
public:
  using AppToken =
      decltype(std::declval<lexec::counting_scope &>().get_token());
  using LeaseProvider = std::function<std::shared_ptr<NativeHostLease>()>;
  VulkanSharedAdapter(tc::DeviceInfo, lqtexec::EventLoopScheduler, AppToken,
                      LeaseProvider, std::function<void()> retireHost,
                      PresentationEvents);
  ~VulkanSharedAdapter() override;
  void probe(tc::DeviceInfo const &,
             std::function<void(Result<ProbeReport>)>) override;
  void preparePath(PresentationPath, std::shared_ptr<NativeHostLease>,
                   Done) override;
  TargetDesc describeTarget(ViewportExtent const &) const override;
  void setFrameContext(SessionGeneration, ViewportExtent const &) override;
  void consumeImage(OwnedFrame) override;
  void beginDrain(Done) override;
  void releasePath(Done) override;
  ViewportExtent resolvedViewport(ViewportExtent) const override;
  PerformanceSnapshot presentationStats() const override;
  std::future<lr::render::ImageRgba8>
  captureNext(); // explicit diagnostic only, never GUI-wait
private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace demo
