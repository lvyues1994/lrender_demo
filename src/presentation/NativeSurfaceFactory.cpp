#define VK_USE_PLATFORM_XCB_KHR
#include "NativeSurfaceFactory.h"
#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QMetaObject>
#include <QPlatformSurfaceEvent>
#include <QThread>
#include <QUuid>
#include <QWidget>
#include <QWindow>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <stdexcept>
#include <xcb/xcb.h>
namespace demo {
NativeSurfacePolicy::NativeSurfacePolicy() : owner_(std::this_thread::get_id()) {}
void NativeSurfacePolicy::requireOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Platform: native host operation must run on its GUI owner thread");
}
VkSurfaceKHR NativeSurfacePolicy::transfer(VkInstance instance, SurfaceCreator const &create) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!alive_ || released_ || destroyed_ || attempted_ || instance == VK_NULL_HANDLE)
            throw std::logic_error("Surface: invalid lease, instance, or duplicate transfer");
        attempted_ = creating_ = true;
    }
    VkSurfaceKHR surface;
    try { surface = create(instance); }
    catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        creating_ = false;
        throw;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    creating_ = false;
    if (surface == VK_NULL_HANDLE)
        throw std::runtime_error("Surface: native creator returned VK_NULL_HANDLE");
    transferred_ = true;
    return surface; // presenter exclusively owns it from here; policy holds no handle
}
void NativeSurfacePolicy::drained() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!transferred_ || destroyed_ || creating_)
        throw std::logic_error("Synchronization: drain outside presenter lifetime");
    drained_ = true;
}
void NativeSurfacePolicy::presenterDestroyed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!drained_ || destroyed_ || creating_)
        throw std::logic_error("Synchronization: presenter destruction before drain");
    destroyed_ = true;
}
void NativeSurfacePolicy::constructionFailed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (creating_ || destroyed_)
        throw std::logic_error("Surface: constructor has not unwound or was already retired");
    drained_ = destroyed_ = true;
}
void NativeSurfacePolicy::requireWorker() const {
    if(std::this_thread::get_id()==owner_) throw std::logic_error("presenter cleanup must not block GUI owner");
}
void NativeSurfacePolicy::setInvalidationHandler(std::function<void(std::uint64_t)> handler) {
    requireOwner(); std::uint64_t serial;
    { std::lock_guard<std::mutex> lock(mutex_); invalidated_=handler;serial=invalidationSerial_; }
    if(handler && serial) {
        try {handler(serial);} catch(std::exception const& e){qWarning()<<"Platform: invalidation observer failed:"<<e.what();}
        catch(...){qWarning()<<"Platform: invalidation observer failed (unknown)";}
    } // late subscriber sees already-invalid lease
}
std::uint64_t NativeSurfacePolicy::invalidationSerial() const {
    std::lock_guard<std::mutex> lock(mutex_);return invalidationSerial_;
}
void NativeSurfacePolicy::invalidate() {
    requireOwner();
    std::function<void(std::uint64_t)> notify;std::uint64_t serial;
    { std::lock_guard<std::mutex> lock(mutex_);
      if(!alive_) return;
      alive_=false;serial=++invalidationSerial_;notify=invalidated_;
    }
    if(notify) {
        try {notify(serial);} catch(std::exception const& e){qWarning()<<"Platform: invalidation observer failed:"<<e.what();}
        catch(...){qWarning()<<"Platform: invalidation observer failed (unknown)";}
    } // GUI, outside lock; owner can stop admission immediately
}
void NativeSurfacePolicy::releaseHost() {
    requireOwner();
    std::lock_guard<std::mutex> lock(mutex_);
    if (creating_ || (transferred_ && !destroyed_))
        throw std::logic_error("Synchronization: host must outlive presenter flush and destruction");
    alive_ = false;
    released_ = true;
}
bool NativeSurfacePolicy::canRelease() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !creating_ && (!transferred_ || destroyed_);
}
bool NativeSurfacePolicy::hostAlive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alive_ && !released_;
}
namespace {
struct Connection {
    xcb_connection_t *value = nullptr;
    ~Connection() { if (value) xcb_disconnect(value); }
};
SurfaceCreator xcbCreator(QWindow &window, std::function<int()> &mapState, std::function<QSize()> &physical) {
  if (QGuiApplication::platformName() != "xcb")
    throw std::runtime_error("Platform: Unsupported Qt platform; only public xcb bridge is validated");
  auto connection = std::make_shared<Connection>();
  connection->value = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(connection->value))
    throw std::runtime_error("Platform: cannot connect to the Qt DISPLAY server");
  auto const id = static_cast<xcb_window_t>(window.winId());
  qInfo() << "native bridge XID=" << id << " title=" << window.title();
  QGuiApplication::sync();
  // Verify this independent public XCB connection resolves the exact Qt window,
  // not an accidental XID on a different DISPLAY. The title contains a unique UUID.
  auto const atomName = QByteArray("_NET_WM_NAME");
  auto atom = xcb_intern_atom_reply(
      connection->value, xcb_intern_atom(connection->value, false, atomName.size(), atomName.constData()),
      nullptr);
  if (!atom)
    throw std::runtime_error("Platform: cannot query Qt window identity");
  auto reply = xcb_get_property_reply(
      connection->value,
      xcb_get_property(connection->value, false, id, atom->atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 256), nullptr);
  std::free(atom);
  QByteArray actual;
  if (reply)
    actual = QByteArray(static_cast<char const *>(xcb_get_property_value(reply)),
                        xcb_get_property_value_length(reply));
  std::free(reply);
  if (actual != window.title().toUtf8())
    throw std::runtime_error("Platform: XCB connection does not identify the same Qt native window");
  auto geometry = xcb_get_geometry_reply(connection->value, xcb_get_geometry(connection->value, id), nullptr);
  if (!geometry)
    throw std::runtime_error("Platform: Qt native window has no XCB geometry");
  std::free(geometry);
  mapState = [connection, id] {
    auto attrs = xcb_get_window_attributes_reply(connection->value,
                                                 xcb_get_window_attributes(connection->value, id), nullptr);
    if (!attrs)
      throw std::runtime_error("Platform: native XCB window attributes unavailable");
    int state = attrs->map_state;
    std::free(attrs);
    return state;
  };
  physical = [connection, id] {
    auto geometry =
        xcb_get_geometry_reply(connection->value, xcb_get_geometry(connection->value, id), nullptr);
    if (!geometry)
      throw std::runtime_error("Platform: native geometry unavailable");
    QSize pixels(geometry->width, geometry->height);
    std::free(geometry);
    return pixels;
  };
  return [connection, id](VkInstance instance) {
    auto create =
        reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"));
    if (!create)
      throw std::runtime_error("Platform: VK_KHR_xcb_surface was not enabled");
    VkXcbSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    info.connection = connection->value;
    info.window = id;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    auto result = create(instance, &info, nullptr, &surface);
    if (result != VK_SUCCESS)
      throw std::runtime_error("Surface: vkCreateXcbSurfaceKHR failed: " + std::to_string(result));
    return surface;
  };
}
} // namespace
NativeSurfaceFactory::NativeSurfaceFactory(SurfaceCreator driverBoundary, QWidget *parent,
                                           bool serializeBoundaryOnGui) {
  requireGui();
  lease_ = std::make_shared<NativeHostLease>();
  lease_->policy = std::make_shared<NativeSurfacePolicy>();
  auto window = std::make_unique<QWindow>();
  window->setSurfaceType(QSurface::RasterSurface); // no Qt renderer, VkInstance or Qt-owned VkSurface
  window->setTitle("lrender native probe " + QUuid::createUuid().toString());
  window_ = window.get();
  container_.reset(QWidget::createWindowContainer(window.release(), parent)); // documented ownership transfer
  container_->resize(requested_);
  container_->show();
  bool const guiBridge = serializeBoundaryOnGui || !driverBoundary;
  if (!driverBoundary) {
    driverBoundary = xcbCreator(*window_, nativeMapState_, nativeExtent_);
    lease_->instanceExtensions = {VK_KHR_XCB_SURFACE_EXTENSION_NAME}; // presenter adds KHR_surface
  }
  auto policy = lease_->policy;
  lease_->createSurface = [policy, create = std::move(driverBoundary), guiBridge](VkInstance instance) {
    if (!guiBridge)
      return policy->transfer(instance, create);
    // Only host-surface creation is serialized with Qt native invalidation.
    // Presenter/device construction stays on the worker. GUI never waits.
    return policy->transfer(instance, [policy, create](VkInstance instance) {
      struct Handshake {
        std::mutex mutex;
        std::condition_variable wake;
        bool ready = false, abandoned = false;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        std::exception_ptr error;
      };
      auto receipt = std::make_shared<Handshake>();
      QMetaObject::invokeMethod(
          qApp,
          [receipt, policy, create, instance] {
            // Keep the borrowed VkInstance alive through the native call: the
            // worker cannot time out/unwind it while this lock is held. A queue
            // timeout abandons only a not-yet-started call, never an active one.
            std::unique_lock<std::mutex> lock(receipt->mutex);
            if (receipt->abandoned)
              return;
            try {
              if (!policy->hostAlive())
                throw std::runtime_error("Platform: invalidated lease before GUI surface creation");
              receipt->surface = create(instance);
            } catch (...) {
              receipt->error = std::current_exception();
            }
            receipt->ready = true;
            lock.unlock();
            receipt->wake.notify_all();
          },
          Qt::QueuedConnection);
      std::unique_lock<std::mutex> lock(receipt->mutex);
      if (!receipt->wake.wait_for(lock, std::chrono::seconds(3), [&] { return receipt->ready; })) {
        receipt->abandoned = true;
        throw std::runtime_error("Platform: surface GUI handshake timeout");
      }
      if (receipt->error)
        std::rethrow_exception(receipt->error);
      return receipt->surface;
    });
  };
  window_->installEventFilter(this);
}
NativeSurfaceFactory::~NativeSurfaceFactory() {
    if (!lease_->policy->canRelease())
        qFatal("NativeSurfaceFactory destroyed before presenter drain/destruction");
    releaseHost();
}
void NativeSurfaceFactory::requireGui() const {
    if (!qApp || QThread::currentThread() != qApp->thread())
        throw std::logic_error("Platform: NativeSurfaceFactory requires the QApplication GUI thread");
}
std::shared_ptr<NativeHostLease> NativeSurfaceFactory::lease() const { requireGui(); return lease_; }
void NativeSurfaceFactory::setViewport(QSize logical) {
    requireGui();
    if (!window_) throw std::logic_error("Platform: released host");
    requested_ = logical;
    if (logical.width() <= 0 || logical.height() <= 0) container_->hide();
    else { container_->resize(logical); container_->show(); }
}
void NativeSurfaceFactory::setVisible(bool visible) {
    requireGui();
    if (!container_) throw std::logic_error("Platform: released host");
    container_->setVisible(visible);
}
QSize NativeSurfaceFactory::physicalExtent() const {
    requireGui();
    if (!window_ || requested_.width() <= 0 || requested_.height() <= 0) return {};
    if (nativeExtent_)
      return nativeExtent_(); // actual XCB pixels, no fractional-DPR rounding guess
    return QSize(qRound(window_->width() * window_->devicePixelRatio()), qRound(window_->height() * window_->devicePixelRatio()));
}
double NativeSurfaceFactory::devicePixelRatio() const { requireGui(); return window_ ? window_->devicePixelRatio() : 1.0; }
bool NativeSurfaceFactory::exposed() const {
    requireGui();
    if (!window_ || !container_) return false;
    // Embedded QWindow::isExposed may lag its native ancestors on X11. Query
    // actual server viewability: it includes both this window and all parents.
    if (nativeMapState_) return nativeMapState_() == XCB_MAP_STATE_VIEWABLE;
    return container_->isVisible() && window_->isExposed();
}
void NativeSurfaceFactory::releaseHost() {
    requireGui();
    lease_->policy->releaseHost();
    container_.reset();
    window_ = nullptr;
}
bool NativeSurfaceFactory::eventFilter(QObject *object, QEvent *event) {
    if (object == window_ && event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        try {lease_->policy->invalidate();}
        catch(std::exception const& e){qWarning()<<"Platform: native event boundary failed:"<<e.what();}
        catch(...){qWarning()<<"Platform: native event boundary failed (unknown)";}
    return QObject::eventFilter(object, event);
}
} // namespace demo
