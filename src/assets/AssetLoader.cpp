#include "AssetLoader.h"
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <lexec/stop_token.hpp>
namespace demo {
namespace {
bool cancelled(RenderError const &e) noexcept {
  return e.stage == ErrorStage::Load && e.recoverable && e.cause == "cancelled";
}
} // namespace
struct AssetLoader::Impl : QObject, std::enable_shared_from_this<Impl> {
  QThreadPool *pool;
  AppToken root;
  Decode decode;
  std::size_t count = 0;
  std::function<void()> idle;
  bool idleQueued = false;
  void notifyIdle() {
    if (idleQueued)
      return;
    idleQueued = true;
    auto self = shared_from_this();
    QTimer::singleShot(0, this, [self] {
      self->idleQueued = false;
      if (!self->count && self->idle) {
        auto callback = self->idle;
        callback();
      }
    });
  }
  std::unique_ptr<lqtexec::ObjectScope> completion;
  struct Request {
    AssetRequestId id;
    lexec::inplace_stop_source stop;
    std::function<void(LoadOutcome)> done;
    std::atomic<bool> delivered{false};
  };
  Impl(QThreadPool *p, lqtexec::EventLoopScheduler gui, AppToken token,
       Decode d)
      : pool(p), root(token), decode(d ? std::move(d) : Decode{decodeAsset}) {
    if (!QCoreApplication::instance() ||
        gui.thread() != QCoreApplication::instance()->thread() || !pool ||
        pool->maxThreadCount() < 1 || pool->maxThreadCount() > 2 ||
        QThread::currentThread() != gui.thread())
      throw std::invalid_argument(
          "AssetLoader requires GUI owner and bounded pool of 1-2 threads");
    completion = std::make_unique<lqtexec::ObjectScope>(this, gui, root);
  }
  void owner() const {
    if (QThread::currentThread() != thread())
      throw std::logic_error("AssetLoader GUI ownership");
  }
  void deliver(std::shared_ptr<Request> r, LoadOutcome outcome) {
    if (r->delivered.exchange(true))
      return;
    auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self, r, outcome = std::move(outcome)]() mutable {
                         self->owner();
                         if (r->stop.stop_requested())
                           outcome = {r->id, LoadStatus::Cancelled, {}, {}};
                         auto callback = std::move(r->done);
                         try {
                           callback(std::move(outcome));
                         } catch (...) {
                           qWarning("AssetLoader observer threw");
                         }
                         --self->count;
                         if (!self->count)
                           self->notifyIdle();
                       });
  }
};
AssetLoader::AssetLoader(QThreadPool *p, lqtexec::EventLoopScheduler gui,
                         AppToken root, Decode decode)
    : impl_(std::make_shared<Impl>(p, gui, root, std::move(decode))) {}
AssetLoader::~AssetLoader() {
  if (QThread::currentThread() != impl_->thread() || impl_->count)
    qFatal("AssetLoader lifetime violation: GUI owner and idle receipts "
           "required before destruction");
}
std::size_t AssetLoader::inFlight() const {
  impl_->owner();
  return impl_->count;
}
void AssetLoader::assertIdle() const {
  impl_->owner();
  if (impl_->count)
    throw std::logic_error("AssetLoader still has producer receipts");
}
void AssetLoader::setIdleHandler(std::function<void()> callback) {
  impl_->owner();
  impl_->idle = std::move(callback);
  if (!impl_->count)
    impl_->notifyIdle();
}
LoadTicket AssetLoader::beginLoad(AssetEntry entry, AssetRequestId request,
                                  std::function<void(LoadOutcome)> done) {
  auto self = impl_;
  self->owner();
  if (!done)
    throw std::invalid_argument("load receipt required");
  auto r = std::make_shared<Impl::Request>();
  r->id = request;
  r->done = std::move(done);
  auto work =
      lexec::write_env(
          lexec::schedule(lqtexec::ThreadPoolScheduler{self->pool}),
          lexec::prop{lexec::get_stop_token, r->stop.get_token()}) |
      lexec::then([self, r, entry = std::move(entry)]() {
        auto value =
            self->decode(entry, [r] { return r->stop.stop_requested(); });
        if (auto e = std::get_if<RenderError>(&value))
          return LoadOutcome{r->id,
                             cancelled(*e) ? LoadStatus::Cancelled
                                           : LoadStatus::Failed,
                             {},
                             *e};
        auto asset = std::get<CpuAssetPtr>(std::move(value));
        if (!asset || asset->model.value != entry.id.value)
          return LoadOutcome{r->id,
                             LoadStatus::Failed,
                             {},
                             RenderError{ErrorStage::Load, false,
                                         "invalid decoder output",
                                         "model/asset mismatch"}};
        return LoadOutcome{r->id, LoadStatus::Loaded, std::move(asset), {}};
      }) |
      lexec::upon_stopped(
          [r] { return LoadOutcome{r->id, LoadStatus::Cancelled, {}, {}}; }) |
      lexec::upon_error([r](std::exception_ptr e) {
        std::string cause = "unknown decoder failure";
        try {
          if (e)
            std::rethrow_exception(e);
        } catch (std::exception const &x) {
          cause = x.what();
        } catch (...) {
        }
        return LoadOutcome{r->id,
                           LoadStatus::Failed,
                           {},
                           RenderError{ErrorStage::Load, false, "decode failed",
                                       std::move(cause)}};
      });
  auto receipt = [self, r](LoadOutcome value) {
    self->deliver(r, std::move(value));
  };
  // Cancel: public API is write_env(get_stop_token) on `work` above.
  // lexec::stop_when exists only as lexec::detail::stop_when (not exported to lexec::).
  // spawn(token) already wraps via token.wrap — do not also associate() (m5).
  auto hopped = lqtexec::then_on(std::move(work), *self->completion, receipt) |
                lexec::upon_stopped([self, r]() noexcept {
                  self->deliver(r, {r->id, LoadStatus::Cancelled, {}, {}});
                }) |
                lexec::upon_error([self, r](std::exception_ptr) noexcept {
                  self->deliver(r, {r->id,
                                    LoadStatus::Failed,
                                    {},
                                    RenderError{ErrorStage::Load, false,
                                                "load dispatch failed",
                                                "completion dispatch"}});
                });
  LoadTicket ticket{request, [r] {
                      r->stop.request_stop();
                    }}; // allocate before producer starts
  ++self->count;
  try {
    lexec::spawn(std::move(hopped), self->root);
  } catch (...) {
    --self->count;
    throw;
  }
  return ticket;
}
} // namespace demo
