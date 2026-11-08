#include "RenderSession.h"
#include "FramePump.h"
#include "SessionFrameSubmit.h"
#include "SessionState.h"
#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <lqtexec/EventLoopContext.h>
#include <map>
namespace demo {
namespace {
lexec::static_thread_pool &cleanupPool() {
  static lexec::static_thread_pool pool{1};
  return pool;
}
template <class F> void post(F f) { QTimer::singleShot(0, QCoreApplication::instance(), std::move(f)); }
RenderError issue(ErrorStage stage, std::string message, std::string cause) {
  return {stage, false, std::move(message), std::move(cause)};
}
struct Resources {
  std::unique_ptr<lr::runtime::Runtime> runtime;
  std::unique_ptr<lr::render::Renderer> renderer;
  std::unique_ptr<lr::scene::World> world;
};
} // namespace
struct RenderSession::Impl : QObject, std::enable_shared_from_this<Impl> {
  enum Phase {
    Idle,
    Preparing,
    Probing,
    Ready,
    Draining,
    DrainFailed,
    Releasing,
    Destroying,
    Joining,
    Stopped
  } phase = Idle;
  Resources resources;
  std::unique_ptr<PresentationAdapter> adapter;
  SubmitFrame overrideSubmit;
  SessionEvents events;
  SessionState state;
  FramePump pump{2};
  std::optional<lr::core::RenderTargetId> target;
  std::optional<lr::core::RenderWorldDelta> pending;
  struct Record {
    FrameStamp stamp;
    lr::core::RenderTargetId target;
  };
  std::map<std::uint64_t, Record> outstanding;
  PerformanceSnapshot performance;
  std::optional<RenderError> lastError;
  bool started = false, stopRequested = false, faulted = false, unknownAcceptance = false, rebuild = false,
       targetFresh = false;
  bool tickQueued = false, noticeQueued = false, resourcesGone = false;
  SessionGeneration candidateGeneration = 0;
  AssetLoader *loader = nullptr;
  std::shared_ptr<Impl> keepAlive;
  lqtexec::EventLoopContext gui;
  lexec::counting_scope root;
  struct JoinReceiver {
    using receiver_concept = lexec::receiver_t;
    std::shared_ptr<Impl> owner;
    void set_value() && noexcept;
  };
  using JoinOp =
      lexec::connect_result_t<decltype(std::declval<lexec::counting_scope &>().join()), JoinReceiver>;
  std::unique_ptr<JoinOp> joinOp;
  std::unique_ptr<lqtexec::ObjectScope> cleanupScope, uiScope;
  Impl(Resources r, std::unique_ptr<PresentationAdapter> a, SubmitFrame submit, SessionEvents ev)
      : resources(std::move(r)), adapter(std::move(a)), overrideSubmit(std::move(submit)),
        events(std::move(ev)) {
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
      throw std::logic_error("RenderSession requires GUI owner");
    if (!resources.runtime || !resources.renderer || !resources.world || !adapter)
      throw std::invalid_argument("RenderSession requires initialized resources and adapter");
    cleanupScope = std::make_unique<lqtexec::ObjectScope>(this, gui.get_scheduler(), root.get_token());
    uiScope = std::make_unique<lqtexec::ObjectScope>(this, gui.get_scheduler(), root.get_token());
  }
  void owner() const {
    if (QThread::currentThread() != thread())
      throw std::logic_error("RenderSession requires serialized GUI owner");
  }
  SessionSnapshotPtr snapshot() const {
    owner();
    auto value = std::make_shared<SessionSnapshot>(*state.snapshot());
    value->performance = performance;
    value->performance.inFlight = pump.inFlight();
    if (lastError)
      value->error = lastError;
    if (resources.renderer) {
      auto const &info = resources.renderer->deviceInfo();
      value->backend = info.backend;
      value->device = info.deviceName;
    }
    return value;
  }
  void publish() {
    if (!uiScope || noticeQueued)
      return;
    noticeQueued = true;
    auto self = shared_from_this();
    lexec::spawn(lqtexec::then_on(lexec::just(), *uiScope,
                                  [self]() noexcept {
                                    self->noticeQueued = false;
                                    try {
                                      auto callback = self->events.onSnapshot;
                                      if (callback)
                                        callback(self->snapshot());
                                    } catch (...) {
                                      qWarning() << "Session: snapshot observer threw";
                                    }
                                  }),
                 root.get_token());
  }
  void report(RenderError error) {
    lastError = error;
    try {
      auto callback = events.onError;
      if (callback)
        callback(error);
    } catch (...) {
      qWarning() << "Session: error observer threw";
    }
    publish();
  }
  void fault(RenderError error) {
    faulted = true;
    pump.setPaused(true);
    state.fail(error);
    report(std::move(error));
  }
  bool blocked() const { return stopRequested || faulted; }
  bool area() const {
    auto e = state.viewport();
    return e.logical.x > 0 && e.logical.y > 0 && e.physical.x > 0 && e.physical.y > 0;
  }
  void queueTick() {
    if (tickQueued || phase == Stopped || phase == Joining)
      return;
    tickQueued = true;
    auto self = shared_from_this();
    post([self] {
      self->tickQueued = false;
      self->tick();
    });
  }
  template <class F> Done control(F f) {
    auto owner = std::make_shared<std::shared_ptr<Impl>>(shared_from_this());
    auto once = std::make_shared<std::atomic<bool>>(false);
    return [owner, once, f = std::move(f)](auto result) mutable {
      if (once->exchange(true))
        return;
      auto self = std::move(*owner);
      post([self = std::move(self), f = std::move(f), result = std::move(result)]() mutable {
        f(*self, std::move(result));
      });
    };
  }
  void prepare() {
    phase = Preparing;
    candidateGeneration = state.snapshot()->generation;
    auto self = shared_from_this();
    adapter->preparePath(
        PresentationPath::Readback, {}, control([](Impl &s, auto result) {
          if (auto e = std::get_if<RenderError>(&result)) {
            s.phase = Ready;
            s.fault(*e);
            s.queueTick();
            return;
          }
          if (s.stopRequested) {
            s.phase = Ready;
            s.queueTick();
            return;
          }
          s.phase = Probing;
          auto once = std::make_shared<std::atomic<bool>>(false);
          auto owner = std::make_shared<std::shared_ptr<Impl>>(s.shared_from_this());
          s.adapter->probe(s.resources.renderer->deviceInfo(), [owner, once](auto result) {
            if (once->exchange(true))
              return;
            auto session = std::move(*owner);
            post([owner = std::move(session), result = std::move(result)]() mutable {
              auto &s = *owner;
              s.phase = Ready;
              if (auto e = std::get_if<RenderError>(&result)) {
                s.fault(*e);
                s.queueTick();
                return;
              }
              if (s.stopRequested || !s.area() || s.state.snapshot()->generation != s.candidateGeneration) {
                s.rebuild = true;
                s.queueTick();
                return;
              }
              try {
                auto desc = s.adapter->describeTarget(s.state.viewport());
                if (!std::holds_alternative<lr::render::ImageTargetDesc>(desc))
                  throw std::runtime_error("P6 requires owned readback target; Vulkan adapter is P9");
                s.target =
                    s.resources.renderer->targets().createImage(std::get<lr::render::ImageTargetDesc>(desc));
                s.adapter->setFrameContext(s.candidateGeneration, s.state.viewport());
                if (!s.state.presentationReady(s.candidateGeneration, PresentationPath::Readback,
                                               s.resources.renderer->deviceInfo().backend,
                                               s.resources.renderer->deviceInfo().deviceName))
                  throw std::runtime_error("stale presentation candidate");
                s.targetFresh = true;
                s.performance.accumulatedSamples = 0;
                s.rebuild = false;
                s.pump.markDirty();
                s.queueTick();
                s.publish();
              } catch (std::exception const &e) {
                s.fault(issue(ErrorStage::Surface, "target preparation failed", e.what()));
              }
            });
          });
        }));
  }
  void destroyTarget() {
    if (target) {
      resources.renderer->targets().destroy(*target);
      target.reset();
    }
    targetFresh = false;
  }
  void beginDrain() {
    phase = Draining;
    adapter->beginDrain(control([](Impl &s, auto result) {
      if (auto e = std::get_if<RenderError>(&result)) {
        s.phase = DrainFailed;
        s.report(*e);
        return;
      }
      try {
        s.destroyTarget();
      } catch (std::exception const &e) {
        s.phase = DrainFailed;
        s.report(issue(ErrorStage::Shutdown, "target destruction failed", e.what()));
        return;
      }
      s.phase = Releasing;
      s.adapter->releasePath(s.control([](Impl &s, auto result) {
        if (auto e = std::get_if<RenderError>(&result)) {
          s.phase = DrainFailed;
          s.report(*e);
          return;
        }
        s.phase = Idle;
        s.rebuild = false;
        if (s.stopRequested) {
          if (s.resourcesGone)
            s.finishScopes();
          else
            s.destroyResources(false);
        } else
          s.queueTick();
      }));
    }));
  }
  void destroyResources(bool ambiguousBarrier) {
    phase = Destroying;
    auto bundle = std::make_shared<Resources>(std::move(resources));
    auto self = shared_from_this();
    auto work = lexec::schedule(cleanupPool().get_scheduler()) | lexec::then([bundle]() noexcept {
                  // External fixed worker: never Runtime's own pool or the Renderer GPU thread.
                  // Renderer destructor joins actual queued producers and clears GPU frames.
                  bundle->renderer.reset();
                  bundle->world.reset();
                  bundle->runtime.reset();
                });
    lexec::spawn(lqtexec::then_on(std::move(work), *cleanupScope,
                                  [self, ambiguousBarrier]() noexcept {
                                    self->resourcesGone = true;
                                    self->pending.reset();
                                    if (ambiguousBarrier) {
                                      // Terminal teardown is evidence, not recovery/resume. Any delayed
                                      // observer is redundant now; target was logically removed while plans
                                      // retained it.
                                      for (auto const &item : self->outstanding)
                                        self->pump.complete(item.second.stamp);
                                      self->outstanding.clear();
                                      self->unknownAcceptance = false;
                                      self->phase = Ready;
                                      self->queueTick();
                                    } else
                                      self->finishScopes();
                                  }),
                 root.get_token());
  }
  void finishScopes() {
    phase = Joining;
    root.close();
    uiScope.reset();
    cleanupScope.reset();
    joinOp =
        std::unique_ptr<JoinOp>(new JoinOp(lexec::connect(root.join(), JoinReceiver{shared_from_this()})));
    lexec::start(*joinOp); // operation retained; completion posts final destruction to GUI
  }
  void tick() {
    if (phase == Stopped || phase == Joining || phase == Destroying || phase == Preparing ||
        phase == Probing || phase == Draining || phase == Releasing || phase == DrainFailed)
      return;
    if (stopRequested) {
      if (unknownAcceptance && !overrideSubmit && pump.inFlight()) {
        // No result receiver could be attached: retire logical target then use actual
        // Renderer destruction as producer barrier. Existing plans own their target.
        destroyTarget();
        destroyResources(true);
        return;
      }
      if (pump.inFlight() == 0)
        beginDrain();
      return;
    }
    if (faulted)
      return;
    if (rebuild) {
      if (pump.inFlight() == 0)
        beginDrain();
      return;
    }
    if (phase == Idle) {
      if (started && area())
        prepare();
      return;
    }
    pump.setPaused(!state.canSubmit());
    if (!target || !state.canSubmit())
      return;
    auto stamp = pump.reserve(lr::core::ViewId{0}, state.snapshot()->generation);
    if (!stamp)
      return;
    outstanding.emplace(stamp->sequence, Record{*stamp, *target});
    lr::render::FrameDesc frame;
    try {
      if (!pending) {
        pending = resources.world->extractDelta();
        ++performance.deltaExtractions;
      }
      frame.delta = *pending;
      lr::render::ViewDesc view;
      auto current = state.snapshot();
      view.id = stamp->view;
      view.target = *target;
      view.camera = current->camera.camera;
      view.pipeline = current->options.pipeline == PipelineKind::Pathtrace
                          ? resources.renderer->pipelines().pathTraceReference()
                          : resources.renderer->pipelines().visibility();
      view.settings.exposure = current->options.exposure;
      view.settings.antiAliasing =
          current->options.taa ? lr::render::AntiAliasing::Taa : lr::render::AntiAliasing::None;
      view.cameraCut = current->options.taa && state.historyResetPending();
      frame.views.push_back(view);
    } catch (std::exception const &e) {
      pump.complete(*stamp);
      outstanding.erase(stamp->sequence);
      fault(issue(ErrorStage::Submit, "pre-submit assembly failed; pending delta retained", e.what()));
      return;
    }
    auto self = shared_from_this();
    auto owner = std::make_shared<std::shared_ptr<Impl>>(self);
    auto once = std::make_shared<std::atomic<bool>>(false);
    // Completed callbacks retained by a seam must not retain the stopped session.
    auto done = [owner, once, expected = *stamp](FrameCompletion completion) {
      if (once->exchange(true))
        return;
      auto self = std::move(*owner);
      post([self, expected, completion = std::move(completion)]() mutable {
        if (completion.stamp.sequence != expected.sequence ||
            completion.stamp.generation != expected.generation || completion.stamp.view != expected.view)
          completion = {expected, issue(ErrorStage::Submit, "mislabeled completion",
                                        "producer callback correlation mismatch")};
        self->completed(std::move(completion));
      });
    };
    try {
      if (overrideSubmit)
        overrideSubmit(std::move(frame), *stamp, std::move(done));
      else
        submitSessionFrame(*resources.renderer, std::move(frame), *stamp, *cleanupScope, root,
                           std::move(done));
      pending.reset();
      targetFresh = false;
      state.acknowledgeHistoryReset();
    } catch (std::exception const &e) {
      unknownAcceptance = true;
      fault(issue(ErrorStage::Submit, "ambiguous submit; resume forbidden", e.what()));
      return;
    } catch (...) {
      unknownAcceptance = true;
      fault(issue(ErrorStage::Submit, "ambiguous submit; resume forbidden", "non-standard submit exception"));
      return;
    }
    publish();
    queueTick(); // single queued wake, never recurse through completion
  }
  void completed(FrameCompletion completion) {
    auto found = outstanding.find(completion.stamp.sequence);
    if (found == outstanding.end())
      return;
    auto record = found->second;
    outstanding.erase(found);
    pump.complete(record.stamp);
    ++performance.completed;
    if (auto e = std::get_if<RenderError>(&completion.result))
      fault(*e);
    else {
      auto result = std::get<lr::render::FrameResult>(std::move(completion.result));
      if (resources.world && !blocked() && !result.lostTextures.empty()) {
        resources.world->resources().resubmitTextures(result.lostTextures);
        pump.markDirty();
      }
      if (!blocked() && state.acceptsFrame(record.stamp)) {
        if (result.views.size() != 1 || result.views[0].view != record.stamp.view ||
            result.views[0].target != record.target)
          fault(issue(ErrorStage::Submit, "invalid frame view result", "view/target routing mismatch"));
        else {
          performance.accumulatedSamples = result.views[0].stats.accumulatedSamples;
          if (result.views[0].image) {
            auto image = std::make_shared<lr::render::ImageRgba8 const>(std::move(*result.views[0].image));
            try {
              adapter->consumeImage({record.stamp, std::move(image)});
            } catch (std::exception const &e) {
              fault(issue(ErrorStage::Present, "image observer failed", e.what()));
            } catch (...) {
              fault(issue(ErrorStage::Present, "image observer failed", "non-standard delivery exception"));
            }
          }
        }
      }
    }
    publish();
    queueTick();
  }
  void historyChange() {
    if (target && !targetFresh && state.historyResetPending() && !rebuild) {
      state.presentationInvalidated({ErrorStage::Surface, true, "history target reset",
                                     "camera/pipeline/TAA change requires fresh target"});
      rebuild = true;
      performance.accumulatedSamples = 0;
    }
  }
  void stop() {
    owner();
    if (phase == Stopped || phase == Joining)
      return;
    stopRequested = true;
    state.requestStop();
    pump.stop();
    if (phase == DrainFailed)
      phase = Ready;
    publish();
    queueTick();
  }
};
void RenderSession::Impl::JoinReceiver::set_value() && noexcept {
  post([owner = std::move(owner)] {
    owner->joinOp.reset();
    owner->phase = Stopped;
    owner->state.stopped();
    try {
      auto callback = owner->events.onStopped;
      if (callback)
        callback();
    } catch (...) {
      qWarning() << "Session: stopped observer threw";
    }
    owner->keepAlive.reset();
  });
}
RenderSession::RenderSession(std::unique_ptr<lr::runtime::Runtime> runtime,
                             std::unique_ptr<lr::render::Renderer> renderer,
                             std::unique_ptr<lr::scene::World> world,
                             std::unique_ptr<PresentationAdapter> adapter, SubmitFrame submit,
                             SessionEvents events)
    : impl_(std::make_shared<Impl>(Resources{std::move(runtime), std::move(renderer), std::move(world)},
                                   std::move(adapter), std::move(submit), std::move(events))) {
  impl_->keepAlive = impl_;
}
RenderSession::~RenderSession() {
  if (impl_) {
    impl_->events = {};
    impl_->uiScope.reset();
    impl_->stop();
  }
}
void RenderSession::start() {
  auto &s = *impl_;
  s.owner();
  if (s.blocked() || s.started)
    return;
  s.started = true;
  s.state.start();
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::selectModel(ModelId id) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  s.state.selectModel(std::move(id));
  s.report(issue(ErrorStage::Load, "asset loading unavailable before P8",
                 s.loader ? "loader injected but P8 decoding not implemented" : "no AssetLoader injected"));
}
void RenderSession::setAssetLoader(AssetLoader &loader) {
  impl_->owner();
  if (!impl_->blocked())
    impl_->loader = &loader;
}
void RenderSession::setCamera(CameraState camera) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  s.state.setCamera(camera);
  s.historyChange();
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::setRenderOptions(RenderOptions options) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  s.state.setRenderOptions(options);
  s.historyChange();
  s.pump.setContinuous(options.pipeline == PipelineKind::Pathtrace || options.taa);
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::setViewport(ViewportExtent viewport) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  auto generation = s.state.snapshot()->generation;
  s.state.setViewport(viewport);
  if (generation != s.state.snapshot()->generation) {
    if (s.target)
      s.rebuild = true;
    s.performance.accumulatedSamples = 0;
    s.pump.markDirty();
    s.queueTick();
    s.publish();
  }
}
void RenderSession::pause() {
  impl_->owner();
  if (!impl_->blocked()) {
    impl_->state.pause();
    impl_->pump.setPaused(true);
    impl_->publish();
  }
}
void RenderSession::resume() {
  impl_->owner();
  if (!impl_->blocked()) {
    impl_->state.resume();
    impl_->pump.markDirty();
    impl_->queueTick();
    impl_->publish();
  }
}
void RenderSession::requestStop() { impl_->stop(); }
SessionSnapshotPtr RenderSession::snapshot() const { return impl_->snapshot(); }
} // namespace demo
