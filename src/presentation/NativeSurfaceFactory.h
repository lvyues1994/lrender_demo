#pragma once
#include <vulkan/vulkan.h>
#include <QSize>
#include <QObject>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
    void drained(); // worker: only after all submit completions and presenter.flush()
    void presenterDestroyed(); // worker: only after last presenter owner has been released
    void constructionFailed(); // worker: constructor unwound and destroyed any adopted surface
    void invalidate(); // GUI native-surface destruction notification
    void releaseHost(); // GUI; refuses while presenter owns a surface
    bool canRelease() const;
    bool hostAlive() const;
private:
    void requireOwner() const;
    std::thread::id owner_;
    mutable std::mutex mutex_;
    bool alive_ = true, attempted_ = false, transferred_ = false, creating_ = false;
    bool drained_ = false, destroyed_ = false, released_ = false;
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
} // namespace demo
