#include "VulkanSharedAdapter.h"
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
namespace demo {
namespace {
lexec::static_thread_pool &presentationWorker() {
  static lexec::static_thread_pool worker{1};
  return worker;
}
RenderError error(ErrorStage stage, char const *message,
                  std::exception_ptr ep) {
  std::string cause = "unknown";
  try {
    if (ep)
      std::rethrow_exception(ep);
  } catch (std::exception const &e) {
    cause = e.what();
  } catch (...) {
  }
  return {stage, true, message, cause};
}
// Called during surface ownership transfer, before presenter takes ownership.
// Query the renderer UUID's real physical device and public WSI caps, not a
// guessed DPR.
lr::core::ivec2 surfaceExtent(VkInstance instance, VkSurfaceKHR surface,
                              tc::DeviceInfo const &info) {
  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS)
    throw std::runtime_error("Platform: enumerate WSI devices failed");
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  for (auto device : devices) {
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(device, &properties);
    if (!info.gpu ||
        !std::equal(info.gpu->device.begin(), info.gpu->device.end(),
                    identity.deviceUUID) ||
        !std::equal(info.gpu->driver.begin(), info.gpu->driver.end(),
                    identity.driverUUID))
      continue;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &caps) !=
        VK_SUCCESS)
      throw std::runtime_error("Platform: WSI caps query failed");
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
      std::cout << "WSI currentExtent=" << caps.currentExtent.width << "x"
                << caps.currentExtent.height << std::endl;
      return {int(caps.currentExtent.width), int(caps.currentExtent.height)};
    }
    return {0, 0};
  }
  throw std::runtime_error(
      "Capability: renderer UUID has no WSI physical device");
}
} // namespace
struct VulkanSharedAdapter::Impl : QObject, std::enable_shared_from_this<Impl> {
  tc::DeviceInfo info;
  AppToken root;
  LeaseProvider provider;
  std::function<void()> retire;
  ReadbackAdapter readback;
  std::unique_ptr<lqtexec::ObjectScope> scope;
  PresentationPath path = PresentationPath::Readback;
  std::shared_ptr<NativeHostLease> lease;
  std::shared_ptr<lr::present::VulkanPresenter> presenter;
  lr::core::ivec2 extent{0};
  SessionGeneration generation = 0;
  PerformanceSnapshot retiredStats;
  Impl(tc::DeviceInfo i, lqtexec::EventLoopScheduler gui, AppToken r,
       LeaseProvider p, std::function<void()> retireHost,
       PresentationEvents events)
      : info(std::move(i)), root(r), provider(std::move(p)),
        retire(std::move(retireHost)), readback(std::move(events)) {
    if (QThread::currentThread() != QCoreApplication::instance()->thread())
      throw std::logic_error("GUI adapter owner required");
    scope = std::make_unique<lqtexec::ObjectScope>(this, gui, root);
  }
  void owner() const {
    if (QThread::currentThread() != thread())
      throw std::logic_error("adapter GUI ownership");
  }
  template <class Work, class Completion>
  void work(Work run, Completion finish) {
    auto self = shared_from_this();
    auto failure = std::make_shared<std::exception_ptr>();
    auto sender =
        lexec::schedule(presentationWorker().get_scheduler()) |
        lexec::then([run = std::move(run), failure]() mutable noexcept {
          try {
            run();
          } catch (...) {
            *failure = std::current_exception();
          }
        });
    lexec::spawn(
        lqtexec::then_on(
            std::move(sender), *scope,
            [self, finish = std::move(finish), failure]() mutable noexcept {
              try {
                finish(*failure);
              } catch (...) {
                qFatal("presentation completion callback escaped");
              }
            }),
        root);
  }
  PerformanceSnapshot stats() const {
    auto result = retiredStats;
    if (presenter) {
      auto p = presenter->stats();
      result.displayed += p.shown;
      result.sharedShown += p.sharedShown;
      result.dropped += p.dropped;
      if(p.shown)result.presenterLatencyMs = p.meanLatencyMs;
    }
    return result;
  }
};
VulkanSharedAdapter::VulkanSharedAdapter(tc::DeviceInfo info,
                                         lqtexec::EventLoopScheduler gui,
                                         AppToken root, LeaseProvider provider,
                                         std::function<void()> retire,
                                         PresentationEvents events)
    : impl_(std::make_shared<Impl>(std::move(info), gui, root,
                                   std::move(provider), std::move(retire),
                                   std::move(events))) {}
VulkanSharedAdapter::~VulkanSharedAdapter() {
  impl_->owner();
  if (impl_->presenter)
    qFatal("adapter destroyed before worker releasePath");
}
void VulkanSharedAdapter::preparePath(PresentationPath path,
                                      std::shared_ptr<NativeHostLease> lease,
                                      Done done) {
  auto s = impl_;
  s->owner();
  s->path = path;
  if (path == PresentationPath::Readback) {
    s->readback.preparePath(path, {}, std::move(done));
    return;
  }
  if (!s->info.gpu || !s->info.importsMemory || !s->info.syncFiles) {
    done(RenderError{ErrorStage::Probe, true, "Capability: shared unavailable",
                     "renderer lacks GPU identity/importMemory/syncFiles"});
    return;
  }
  try {
    s->lease = lease ? std::move(lease) : s->provider();
    if (!s->lease || !s->lease->policy->hostAlive())
      throw std::runtime_error("Platform: invalid native host lease");
  } catch (...) {
    done(error(ErrorStage::Surface, "Platform: native preparation failed",
               std::current_exception()));
    return;
  }
  auto candidate =
      std::make_shared<std::shared_ptr<lr::present::VulkanPresenter>>();
  auto actual = std::make_shared<lr::core::ivec2>(0);
  s->work(
      [s, candidate, actual] {
        lr::present::VulkanPresenterConfig config;
        config.instanceExtensions = s->lease->instanceExtensions;
        config.shareBuffers = true;
        config.deviceNameContains = s->info.deviceName;
        config.createSurface = [s, actual](VkInstance instance) {
          auto surface = s->lease->createSurface(instance);
          try {
            *actual = surfaceExtent(instance, surface, s->info);
          } catch (...) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            throw;
          }
          return surface;
        };
        *candidate = NativePresenterCleanup::construct(s->lease->policy, [&] {
          return lr::present::makeVulkanPresenter(std::move(config));
        });
      },
      [s, candidate, actual, done](std::exception_ptr failure) {
        if (failure) {
          done(error(ErrorStage::Surface, "presenter construction failed",
                     failure));
          return;
        }
        s->presenter = std::move(*candidate);
        s->extent = *actual;
        done(std::monostate{});
      });
}
void VulkanSharedAdapter::probe(tc::DeviceInfo const &info,
                                std::function<void(Result<ProbeReport>)> done) {
  auto s = impl_;
  s->owner();
  if (s->path == PresentationPath::Readback) {
    s->readback.probe(info, std::move(done));
    return;
  }
  bool shared = s->presenter && info.gpu && info.importsMemory &&
                info.syncFiles && s->presenter->sharesBuffersWith(info);
  std::cout << "Shared probe renderer=" << info.deviceName
            << " importsMemory=" << info.importsMemory
            << " syncFiles=" << info.syncFiles
            << " sharesBuffersWith=" << shared << " UUID=";
  if (info.gpu)
    for (auto b : info.gpu->device)
      std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
  std::cout << std::dec << std::endl;
  done(ProbeReport{
      shared,
      shared
          ? "UUID+driver/external-memory/sync-file verified"
          : "Capability: real sharesBuffersWith/UUID/import/sync unavailable",
      s->presenter ? s->presenter->deviceName() : ""});
}
TargetDesc VulkanSharedAdapter::describeTarget(ViewportExtent const &v) const {
  auto s = impl_;
  s->owner();
  if (s->path == PresentationPath::Readback)
    return s->readback.describeTarget(v);
  if (!s->presenter || !s->lease->policy->hostAlive())
    throw std::runtime_error("invalid shared host");
  return lr::render::WindowTargetDesc{resolvedViewport(v).physical,
                                      s->presenter};
}
ViewportExtent VulkanSharedAdapter::resolvedViewport(ViewportExtent v) const {
  auto s = impl_;
  s->owner();
  if (s->path == PresentationPath::VulkanShared && s->extent.x > 0 &&
      s->extent.y > 0)
    v.physical = s->extent;
  return v;
}
void VulkanSharedAdapter::setFrameContext(SessionGeneration gen,
                                          ViewportExtent const &v) {
  auto s = impl_;
  s->owner();
  if (gen < s->generation)
    throw std::invalid_argument("presentation generation regressed");
  s->generation = gen;
  if (s->path == PresentationPath::Readback)
    s->readback.setFrameContext(gen, v);
  else if (v.physical.x > 0 && v.physical.y > 0)
    s->extent = v.physical; // soft resize uses live host physical/WSI extent
}
void VulkanSharedAdapter::consumeImage(OwnedFrame f) {
  impl_->owner();
  if (impl_->path != PresentationPath::Readback)
    throw std::logic_error("shared path returned readback image");
  impl_->readback.consumeImage(std::move(f));
}
void VulkanSharedAdapter::beginDrain(Done done) {
  auto s = impl_;
  s->owner();
  if (s->path == PresentationPath::Readback) {
    s->readback.beginDrain(std::move(done));
    return;
  }
  if (!s->presenter) {
    done(std::monostate{});
    return;
  }
  s->work(
      [s] {
        auto start = std::chrono::steady_clock::now();
        NativePresenterCleanup::drain(s->lease->policy, s->presenter);
        std::cout << "cleanup phase=flush wallMs="
                  << std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count()
                  << std::endl;
      },
      [done](std::exception_ptr failure) {
        if (failure)
          done(error(ErrorStage::Shutdown, "presenter drain failed", failure));
        else
          done(std::monostate{});
      });
}
void VulkanSharedAdapter::releasePath(Done done) {
  auto s = impl_;
  s->owner();
  if (s->path == PresentationPath::Readback) {
    s->readback.releasePath(std::move(done));
    return;
  }
  auto previousStats = s->retiredStats;
  s->retiredStats = s->stats();
  auto presenter =
      std::make_shared<std::shared_ptr<lr::present::VulkanPresenter>>(
          std::move(s->presenter));
  s->work(
      [s, presenter] {
        if (*presenter)
          NativePresenterCleanup::run(
              s->lease->policy, *presenter, [] {}, [] {},
              [](char const *phase, double ms) {
                std::cout << "cleanup phase=" << phase << " wallMs=" << ms
                          << std::endl;
              });
      },
      [s, presenter, done, previousStats](std::exception_ptr failure) {
        if (failure) {
          s->presenter = std::move(*presenter);
          if (s->presenter)
            s->retiredStats = previousStats;
          done(
              error(ErrorStage::Shutdown, "presenter release failed", failure));
          return;
        }
        if (s->retire)
          s->retire();
        s->lease.reset();
        s->extent = {0, 0};
        done(std::monostate{});
      });
}
PerformanceSnapshot VulkanSharedAdapter::presentationStats() const {
  impl_->owner();
  return impl_->stats();
}
std::future<lr::render::ImageRgba8> VulkanSharedAdapter::captureNext() {
  impl_->owner();
  if (!impl_->presenter)
    throw std::logic_error("capture requires shared presenter");
  return impl_->presenter->captureNext();
}
} // namespace demo
