#pragma once
#include <vulkan/vulkan.h>
#include <QSize>
#include <QObject>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
class QWindow;
class QWidget;
namespace demo {
using SurfaceCreator = std::function<VkSurfaceKHR(VkInstance)>;
// This is the actual factory lifecycle policy; not a simulated presenter.
class NativeSurfacePolicy {
public:
    NativeSurfacePolicy();
    VkSurfaceKHR transfer(VkInstance instance, SurfaceCreator const &create);
    void setInvalidationHandler(std::function<void(std::uint64_t)>);
    std::uint64_t invalidationSerial() const;
    void invalidate(); // GUI native-surface destruction notification
    void releaseHost(); // GUI; refuses while presenter owns a surface
    bool canRelease() const;
    bool hostAlive() const;
private:
    friend struct NativePresenterCleanup;
    void requireOwner() const;
    void requireWorker() const;
    void constructionFailed();
    void drained();
    void presenterDestroyed();
    std::thread::id owner_;
    mutable std::mutex mutex_;
    bool alive_ = true, attempted_ = false, transferred_ = false, creating_ = false;
    bool drained_ = false, destroyed_ = false, released_ = false;
    std::uint64_t invalidationSerial_ = 0;
    std::function<void(std::uint64_t)> invalidated_;
};
struct NativeHostLease {
    std::vector<std::string> instanceExtensions;
    SurfaceCreator createSurface;
    std::shared_ptr<NativeSurfacePolicy> policy;
};
// GUI owns this object and its QWidget container/QWindow until releaseHost succeeds.
// The lease pins native connection metadata, never transfers Qt window ownership.
class NativeSurfaceFactory final : public QObject {
public:
    explicit NativeSurfaceFactory(SurfaceCreator driverBoundary = {});
    ~NativeSurfaceFactory() override;
    std::shared_ptr<NativeHostLease> lease() const;
    void setViewport(QSize logical);
    void setVisible(bool visible);
    QSize physicalExtent() const;
    double devicePixelRatio() const;
    bool exposed() const;
    void releaseHost();
protected:
    bool eventFilter(QObject *, QEvent *) override;
private:
    void requireGui() const;
    std::shared_ptr<NativeHostLease> lease_;
    std::unique_ptr<QWidget> container_;
    QWindow *window_ = nullptr;
    QSize requested_{160, 120};
    std::function<int()> nativeMapState_;
};
// Worker-only ordered resource teardown. Primitive acknowledgements are private:
// callers cannot report a flushed/destroyed presenter without passing this flow.
// Settle MUST finish all submitted senders (even failed ones); ReleaseOwners drops
// Session targets/renderer owners AFTER flush. Host destruction stays on GUI.
struct NativePresenterCleanup {
 template<class Make> static auto construct(std::shared_ptr<NativeSurfacePolicy> const& policy, Make make) -> decltype(make()) {
  policy->requireWorker();
  try {return make();}
  catch(...) {policy->constructionFailed();throw;}
 }
 template<class Presenter,class Settle,class ReleaseOwners>
 static void run(std::shared_ptr<NativeSurfacePolicy> const& policy, std::shared_ptr<Presenter>& presenter,
                 Settle settle,ReleaseOwners releaseOwners) {
  policy->requireWorker();
  if(!presenter) throw std::logic_error("cleanup requires a presenter");
  settle();
  presenter->flush();
  policy->drained();
  releaseOwners();
  if(presenter.use_count()!=1) throw std::logic_error("presenter still has external owners after target teardown");
  presenter.reset();
  policy->presenterDestroyed();
 }
};
} // namespace demo
