#include "RenderSession.h"
#include "AssetInstall.h"
#include "editor/EditorScene.h"
#include "Diagnostics.h"
#include "FramePump.h"
#include "SessionFrameSubmit.h"
#include "workflows/SessionWorkflows.h"
#include "SessionState.h"
#include "assets/AssetLoader.h"
#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <lqtexec/EventLoopContext.h>
#include <map>
#include <string>
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
    bool texturePayload = false, textureSettling = false;
  };
  std::map<std::uint64_t, Record> outstanding;
  PerformanceSnapshot performance;
  mutable Diagnostics diagnostics;
  float ambient = .25f, roughness = .5f;
  bool normalMapping = true;
  bool textureFollowupPending = false;
  std::optional<std::chrono::steady_clock::time_point> receiptWaitStarted;
  double receiptWaitMs = 0;
  std::optional<RenderError> lastError;
  bool started = false, stopRequested = false, faulted = false, unknownAcceptance = false, rebuild = false,
       hardRebuild = false, pathPrepared = false, targetFresh = false;
  bool tickQueued = false, noticeQueued = false, resourcesGone = false;
  SessionGeneration candidateGeneration = 0;
  PresentationPath selectedPath = PresentationPath::Readback;
  bool allowFallback = false, visible = true;
  std::vector<std::string> presentationWarnings;
  AssetLoader *loader = nullptr; // injection outlives this session through onStopped
  std::unique_ptr<AssetInstall> installer;
  editor::EditorScene editor;
  std::map<AssetRequestId, LoadTicket> loads;
  std::optional<AssetEntry> pendingLoad;
  CpuAssetPtr readyAsset;
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
        events(std::move(ev)), diagnostics(resources.renderer && bool(resources.renderer->deviceInfo().gpu)) {
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
      throw std::logic_error("RenderSession requires GUI owner");
    if (!resources.runtime || !resources.renderer || !resources.world || !adapter)
      throw std::invalid_argument("RenderSession requires initialized resources and adapter");
    installer = std::make_unique<AssetInstall>(*resources.world);
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
    value->ambient = ambient;
    value->roughness = roughness;
    value->normalMapping = normalMapping;
    auto display = adapter->presentationStats();
    value->performance.displayed += display.displayed;
    value->performance.sharedShown = display.sharedShown;
    value->performance.dropped = display.dropped;
    value->performance.presenterLatencyMs = display.presenterLatencyMs;
    // GUI observation samples adapter counters/latency into diagnostics. This
    // does not submit/retire GPU work, mutate World, or schedule a frame; repeated
    // snapshot observations are permitted (including command acknowledgements).
    diagnostics.recordPresentation(value->performance.displayed, display.dropped, display.presenterLatencyMs);
    auto measured = diagnostics.snapshot();
    value->performance.completionFps = measured.completionFps;
    value->performance.windowSeconds = measured.windowSeconds;
    value->performance.hostPrepareMs = measured.hostPrepareMs;
    value->performance.frameWallMs = measured.frameWallMs;
    value->performance.gpuPassTotalMs = measured.gpuPassTotalMs;
    value->performance.presenterLatencyMs = measured.presenterLatencyMs;
    value->warnings.insert(value->warnings.end(), presentationWarnings.begin(), presentationWarnings.end());
    value->installedModel = installer && installer->current()
                                ? std::optional<ModelId>{installer->current()->model}
                                : std::nullopt;
    value->assetInstalling = installer && installer->active();
    if (installer && installer->current()) {
      value->assetBounds = installer->current()->bounds;
      value->assetNotices = installer->current()->notices;
      value->warnings.insert(value->warnings.end(), installer->current()->warnings.begin(),
                             installer->current()->warnings.end());
    }
    value->installationParts = installer ? installer->completedParts() : 0;
    value->assetLoadsInFlight = loads.size();
    value->performance.inFlight = pump.inFlight();
    if (lastError)
      value->error = lastError;
    if (resources.renderer) {
      auto const &info = resources.renderer->deviceInfo();
      value->backend = info.backend;
      value->device = info.deviceName;
    }
    publishEditorEntities(*value);
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
    if ((stopRequested || rebuild) && pump.inFlight() && !receiptWaitStarted)
      receiptWaitStarted = std::chrono::steady_clock::now();
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
  // M1: upon_error may run off-GUI; always hop before mutating phase / report.
  void guiDrainFault(char const *message, char const *cause) {
    auto self = shared_from_this();
    post([self, message, cause] {
      self->owner();
      if (self->phase == Draining || self->phase == Releasing || self->phase == DrainFailed) {
        self->phase = DrainFailed;
        self->report(issue(ErrorStage::Shutdown, message, cause));
      }
    });
  }
  lqtexec::ObjectScope &drainHopScope() {
    // Prefer cleanupScope: ~RenderSession resets uiScope before stop()/beginDrain.
    if (cleanupScope)
      return *cleanupScope;
    if (uiScope)
      return *uiScope;
    throw std::logic_error("drain hop scope missing");
  }
  void presentationError(RenderError error) {
    if (selectedPath == PresentationPath::VulkanShared && allowFallback && !stopRequested) {
      presentationWarnings.push_back(error.message + ": " + error.cause);
      selectedPath = PresentationPath::Readback;
      state.presentationInvalidated(error);
      hardRebuild = true;
      rebuild = true;
      publish();
      queueTick(); // old path must drain/release before readback prepare
    } else {
      fault(error);
      queueTick();
    }
  }
  void prepare() {
    phase = Preparing;
    candidateGeneration = state.snapshot()->generation;
    auto self = shared_from_this();
    // m4: preparePath via DoneOpSender + then_on (same Done contract as drain/release).
    auto chain =
        workflows::preparePathSender(*adapter, selectedPath, {}) |
        lqtexec::then_on(drainHopScope(), [self](Result<std::monostate> result) {
          if (auto e = std::get_if<RenderError>(&result)) {
            self->phase = Ready;
            self->presentationError(*e);
            return;
          }
          if (self->stopRequested) {
            self->phase = Ready;
            self->queueTick();
            return;
          }
          self->phase = Probing;
          auto once = std::make_shared<std::atomic<bool>>(false);
          auto owner = std::make_shared<std::shared_ptr<Impl>>(self);
          self->adapter->probe(self->resources.renderer->deviceInfo(), [owner, once](auto result) {
            if (once->exchange(true))
              return;
            auto session = std::move(*owner);
            post([owner = std::move(session), result = std::move(result)]() mutable {
              auto &s = *owner;
              s.phase = Ready;
              if (auto e = std::get_if<RenderError>(&result)) {
                s.presentationError(*e);
                return;
              }
              auto const report = std::get<ProbeReport>(result);
              if (s.selectedPath == PresentationPath::VulkanShared && !report.shared) {
                s.presentationError({ErrorStage::Probe, true, "Capability: shared rejected", report.reason});
                return;
              }
              if (s.stopRequested || !s.area() || s.state.snapshot()->generation != s.candidateGeneration) {
                s.rebuild = true;
                s.queueTick();
                return;
              }
              try {
                auto resolved = s.adapter->resolvedViewport(s.state.viewport());
                auto old = s.state.viewport();
                if (resolved.physical.x != old.physical.x || resolved.physical.y != old.physical.y) {
                  s.state.setViewport(resolved);
                  s.candidateGeneration = s.state.snapshot()->generation;
                }
                auto desc = s.adapter->describeTarget(s.state.viewport());
                if (auto image = std::get_if<lr::render::ImageTargetDesc>(&desc))
                  s.target = s.resources.renderer->targets().createImage(*image);
                else
                  s.target = s.resources.renderer->targets().createWindow(
                      std::get<lr::render::WindowTargetDesc>(desc));
                s.adapter->setFrameContext(s.candidateGeneration, s.state.viewport());
                if (!s.state.presentationReady(s.candidateGeneration, s.selectedPath,
                                               s.resources.renderer->deviceInfo().backend,
                                               s.resources.renderer->deviceInfo().deviceName))
                  throw std::runtime_error("stale presentation candidate");
                s.targetFresh = true;
                s.performance.accumulatedSamples = 0;
                s.rebuild = false;
                s.hardRebuild = false;
                s.pathPrepared = true;
                s.pump.markDirty();
                s.queueTick();
                s.publish();
              } catch (std::exception const &e) {
                s.presentationError(issue(ErrorStage::Surface, "target preparation failed", e.what()));
              }
            });
          });
        });
    lexec::spawn(std::move(chain) | lexec::upon_error([self](std::exception_ptr) noexcept {
                   // Hop to GUI: prepare faults stay in Ready + presentationError, not DrainFailed.
                   post([self] {
                     self->owner();
                     self->phase = Ready;
                     self->presentationError(
                         issue(ErrorStage::Surface, "preparePath dispatch failed", "exception"));
                   });
                 }),
                 root.get_token());
  }
  bool bindFreshTarget() {
    if (stopRequested || !area() || !visible || !pathPrepared)
      return false;
    candidateGeneration = state.snapshot()->generation;
    auto resolved = adapter->resolvedViewport(state.viewport());
    auto old = state.viewport();
    if (resolved.physical.x != old.physical.x || resolved.physical.y != old.physical.y)
      state.setViewport(resolved);
    candidateGeneration = state.snapshot()->generation;
    auto desc = adapter->describeTarget(state.viewport());
    if (auto image = std::get_if<lr::render::ImageTargetDesc>(&desc))
      target = resources.renderer->targets().createImage(*image);
    else
      target = resources.renderer->targets().createWindow(std::get<lr::render::WindowTargetDesc>(desc));
    adapter->setFrameContext(candidateGeneration, state.viewport());
    if (!state.presentationReady(candidateGeneration, selectedPath,
                                 resources.renderer->deviceInfo().backend,
                                 resources.renderer->deviceInfo().deviceName))
      throw std::runtime_error("stale soft presentation candidate");
    if (const char *log = std::getenv("DEMO_LOG_SOFT_EXTENT"); log && log[0] && log[0] != '0')
      std::cout << "soft target extent=" << state.viewport().physical.x << "x"
                << state.viewport().physical.y << " logical=" << state.viewport().logical.x
                << "x" << state.viewport().logical.y << std::endl;
    targetFresh = true;
    performance.accumulatedSamples = 0;
    pump.markDirty();
    return true;
  }
  void softRecreateTarget() {
    // Keep host/presenter (or readback path) alive so the last swapchain/image remains
    // visible until the replacement target presents. Hard teardown still used for native
    // loss, path changes, and final shutdown. Callers only enter when pathPrepared && Ready.
    if (!pathPrepared || phase != Ready)
      qFatal("soft recreate requires prepared Ready presentation path");
    auto finish = [this] {
      try {
        destroyTarget();
        rebuild = false;
        if (bindFreshTarget()) {
          phase = Ready;
          queueTick();
          publish();
          return;
        }
        phase = Ready;
        queueTick();
        publish();
      } catch (std::exception const &e) {
        hardRebuild = true;
        rebuild = true;
        presentationError(issue(ErrorStage::Surface, "soft target rebuild failed", e.what()));
      }
    };
    if (selectedPath == PresentationPath::VulkanShared) {
      phase = Draining;
      auto self = shared_from_this();
      // Soft: drain | then_on(GUI, destroy+bind). Never releasePath (no black flash).
      auto chain = workflows::softDrainOnly(*adapter) |
                   lqtexec::then_on(drainHopScope(), [self, finish](Result<std::monostate> result) {
                     if (self->phase != Draining)
                       return; // superseded / cancelled rebuild
                     if (auto e = std::get_if<RenderError>(&result)) {
                       self->phase = DrainFailed;
                       self->report(*e);
                       // Fail-closed: retain target/path until explicit retry (resume+stop).
                       return;
                     }
                     finish();
                   });
      lexec::spawn(std::move(chain) | lexec::upon_error([self](std::exception_ptr) noexcept {
                     self->guiDrainFault("soft drain dispatch failed", "exception");
                   }),
                   root.get_token());
      return;
    }
    finish();
  }
  void destroyTarget() {
    if (target) {
      auto started = std::chrono::steady_clock::now();
      resources.renderer->targets().destroy(*target);
      target.reset();
      std::cout
          << "cleanup phase=targetDestroy wallMs="
          << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
          << std::endl;
    }
    targetFresh = false;
  }
  void beginDrain() {
    std::cout << "cleanup phase=waitSubmitReceipts wallMs=" << receiptWaitMs
              << " inFlight=" << pump.inFlight() << std::endl;
    receiptWaitMs = 0;
    receiptWaitStarted.reset();
    phase = Draining;
    auto self = shared_from_this();
    // Hard: drain | then_on(destroy) | releasePathSender | then_on(create/finish-stop).
    auto chain = workflows::drainThenOnGui(*adapter, drainHopScope(), [self](Result<std::monostate> drainResult) {
      if (self->phase != Draining)
        return;
      if (auto e = std::get_if<RenderError>(&drainResult)) {
        self->phase = DrainFailed;
        self->report(*e);
        // Fail-closed: retain target until explicit retry — do not destroyResources here.
        return;
      }
      try {
        self->phase = Releasing;
        self->destroyTarget();
      } catch (std::exception const &ex) {
        self->phase = DrainFailed;
        self->report(issue(ErrorStage::Shutdown, "target destruction failed", ex.what()));
        return;
      }
      auto release = workflows::releaseThenOnGui(*self->adapter, self->drainHopScope(), [self](Result<std::monostate> releaseResult) {
        if (self->phase != Releasing)
          return;
        if (auto e = std::get_if<RenderError>(&releaseResult)) {
          self->phase = DrainFailed;
          self->report(*e);
          // Fail-closed: retain host until explicit retry.
          return;
        }
        self->phase = Idle;
        self->rebuild = false;
        self->hardRebuild = false;
        self->pathPrepared = false;
        if (self->stopRequested) {
          if (self->resourcesGone)
            self->finishScopes();
          else
            self->destroyResources(false);
        } else
          self->queueTick(); // Idle → prepare() creates fresh target/path
      });
      lexec::spawn(std::move(release) | lexec::upon_error([self](std::exception_ptr) noexcept {
                     self->guiDrainFault("release dispatch failed", "exception");
                   }),
                   self->root.get_token());
    });
    lexec::spawn(std::move(chain) | lexec::upon_error([self](std::exception_ptr) noexcept {
                   self->guiDrainFault("hard drain dispatch failed", "exception");
                 }),
                 root.get_token());
  }
  void destroyResources(bool ambiguousBarrier) {
    phase = Destroying;
    // All load receipts retired; only GUI touches World, even during terminal cleanup.
    if (installer) {
      editor.clearForModelSwitch();
      // World may still be alive here; flush editor-created destroys before teardown.
      // m-b: never let sync logic_error hang shutdown — fail-closed and continue.
      if (resources.world) {
        try {
          editor.syncToWorld(*resources.world);
        } catch (std::exception const &e) {
          report(issue(ErrorStage::Shutdown, "editor sync during teardown failed closed", e.what()));
        }
      }
    }
    installer.reset();
    auto bundle = std::make_shared<Resources>(std::move(resources));
    auto self = shared_from_this();
    auto work =
        lexec::schedule(cleanupPool().get_scheduler()) | lexec::then([bundle]() noexcept {
          // External fixed worker: never Runtime's own pool or the Renderer GPU thread.
          // Renderer destructor joins actual queued producers and clears GPU frames.
          auto measured = [](char const *phase, auto f) {
            auto start = std::chrono::steady_clock::now();
            f();
            std::cout
                << "cleanup phase=" << phase << " wallMs="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                << std::endl;
          };
          measured("rendererDestructor", [&] { bundle->renderer.reset(); });
          measured("worldDestructor", [&] { bundle->world.reset(); });
          measured("runtimeDestructor", [&] { bundle->runtime.reset(); });
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
  void beginPendingLoad() {
    if (!pendingLoad || blocked())
      return;
    auto entry = std::move(*pendingLoad);
    pendingLoad.reset();
    auto request = state.snapshot()->request;
    auto weak = weak_from_this();
    try {
      // Journal allocation precedes launch. Assignment of the returned ticket is
      // nonthrowing; a successful producer can never become an untracked job.
      auto slot = loads.try_emplace(request, LoadTicket{request, {}}).first;
      auto ticket = loader->beginLoad(std::move(entry), request, [weak](LoadOutcome outcome) {
        if (auto self = weak.lock()) {
          self->owner();
          self->loads.erase(outcome.request);
          if (!self->blocked() && self->state.acceptsAsset(outcome.request)) {
            if (outcome.status == LoadStatus::Loaded)
              self->readyAsset = std::move(outcome.asset);
            else if (outcome.status == LoadStatus::Failed && outcome.error)
              self->report(*outcome.error);
          }
          self->publish();
          self->queueTick();
        }
      });
      slot->second = std::move(ticket);
    } catch (std::exception const &e) {
      loads.erase(request);
      report(issue(ErrorStage::Load, "load start failed", e.what()));
    }
    publish();
  }

  EditorEntityId toId(entt::entity e) const {
    return EditorEntityId{static_cast<std::uint32_t>(entt::to_integral(e))};
  }
  entt::entity fromId(EditorEntityId id) const {
    return entt::entity{id.value};
  }

  void publishEditorEntities(SessionSnapshot &value) const {
    value.entities.clear();
    for (auto e : editor.registry().view<editor::Name, editor::LocalTransform, editor::Hierarchy>()) {
      EditorEntitySnapshot snap;
      snap.id = toId(e);
      snap.name = editor.registry().get<editor::Name>(e).value;
      snap.local = editor.registry().get<editor::LocalTransform>(e).value;
      auto parent = editor.registry().get<editor::Hierarchy>(e).parent;
      if (parent)
        snap.parent = toId(*parent);
      snap.installed = editor.registry().all_of<editor::Installed>(e);
      value.entities.push_back(std::move(snap));
    }
  }

  // N3: do NOT clear the editor at install start — keep old model editable/visible
  // until commit succeeds. Only roll back an in-flight staging transaction.
  void prepareEditorForInstall() {
    if (installer && installer->active())
      installer->rollback();
  }

  void mirrorInstalledIntoEditor() {
    if (!installer)
      return;
    auto asset = installer->current();
    if (!asset)
      return;
    if (!resources.world)
      throw std::logic_error("mirrorInstalledIntoEditor: World required");
    auto const &ents = installer->installedEntities();
    auto const &meshes = installer->installedMeshes();
    auto const &mats = installer->installedPartMaterials();
    if (ents.size() != meshes.size() || ents.size() != mats.size())
      throw std::logic_error("AssetInstall entity/mesh/material size mismatch");
    std::vector<std::string> names;
    names.reserve(ents.size());
    for (std::size_t i = 0; i < ents.size(); ++i)
      names.push_back(asset->model.value + "/part" + std::to_string(i));
    // I2: World is SoT for parent/local transform of installed (and every WorldLink).
    editor.remirrorInstalledFromWorld(*resources.world, ents, names, meshes, mats);
  }

  void adoptInstalledIntoEditor() {
    // Commit path: installer already released previous installed World entities.
    editor.clearForModelSwitch();
    if (resources.world)
      editor.syncToWorld(*resources.world);
    mirrorInstalledIntoEditor();
  }

  // After cancelled/failed install: staging rolled back; World still has previous
  // installed. N3: do NOT clearForModelSwitch (that would destroy editor-created
  // entities). Drop Installed mirrors only, then re-adopt installer->installed.
  void readoptInstalledAfterRollback() {
    // I3/I2: EditorScene::readoptInstalledFromWorld is the single sequence
    // (sync Dirty → forget Installed → remirror from World SoT).
    if (!installer || !resources.world)
      return;
    auto asset = installer->current();
    if (!asset)
      return;
    auto const &ents = installer->installedEntities();
    auto const &meshes = installer->installedMeshes();
    auto const &mats = installer->installedPartMaterials();
    if (ents.size() != meshes.size() || ents.size() != mats.size())
      throw std::logic_error("AssetInstall entity/mesh/material size mismatch");
    std::vector<std::string> names;
    names.reserve(ents.size());
    for (std::size_t i = 0; i < ents.size(); ++i)
      names.push_back(asset->model.value + "/part" + std::to_string(i));
    editor.readoptInstalledFromWorld(*resources.world, ents, names, meshes, mats);
    // I6: sync/remirror mutated World; schedule extract (Visibility is not continuous).
    pump.markDirty();
    queueTick();
  }

  bool installStep() {
    if (!readyAsset && !installer->active())
      return false;
    // No extraction/submit between first staged mutation and commit/rollback.
    // Existing accepted work is not canceled, and must settle before mutation.
    if (pump.inFlight())
      return true;
    if (readyAsset) {
      auto candidate = std::move(readyAsset);
      readyAsset.reset();
      try {
        prepareEditorForInstall();
        installer->begin(std::move(candidate));
      } catch (std::invalid_argument const &e) {
        // begin's preflight precedes all World mutations: old scene remains usable.
        report({ErrorStage::Load, true, "asset preflight rejected", e.what()});
        queueTick();
        return true;
      } catch (std::exception const &e) {
        fault(issue(ErrorStage::Load, "asset preflight failed closed", e.what()));
        return true;
      }
    }
    try {
      if (installer->step(1)) {
        installer->commit();
        // E2: registry becomes authoring SoT for the installed model (World already filled).
        adoptInstalledIntoEditor();
        if (target) {
          state.presentationInvalidated(
              {ErrorStage::Surface, true, "asset committed", "fresh history after atomic scene replacement"});
          rebuild = true;
        }
        performance.accumulatedSamples = 0;
        pump.markDirty();
      }
      publish();
      queueTick(); // bounded one part per GUI turn, not a polling wait
    } catch (std::exception const &e) {
      // Upstream resource allocators do not promise strong bad_alloc atomicity.
      // Fail closed: never extract uncertain mutations or retry a resource delta.
      readyAsset.reset();
      if (installer && installer->active())
        installer->rollback();
      try {
        readoptInstalledAfterRollback(); // N3/I1: restore editor↔World for still-installed model
      } catch (std::exception const &re) {
        // M1: readopt must not escape — fail closed with the original install error.
        fault(issue(ErrorStage::Load, "asset installation failed closed; readopt also failed",
                    std::string(e.what()) + "; readopt: " + re.what()));
        return true;
      }
      fault(issue(ErrorStage::Load, "asset installation failed closed", e.what()));
    }
    return true;
  }
  void tick() {
    beginPendingLoad();
    if (phase == Stopped || phase == Joining || phase == Destroying || phase == Preparing ||
        phase == Probing || phase == Draining || phase == Releasing || phase == DrainFailed)
      return;
    if (stopRequested) {
      if (!loads.empty())
        return; // running CPU producers must actually retire
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
    if (installStep())
      return;
    if (rebuild || hardRebuild) {
      if (pump.inFlight() == 0) {
        if (rebuild && !hardRebuild && pathPrepared && phase == Ready && !stopRequested)
          softRecreateTarget();
        else
          beginDrain();
      }
      return;
    }
    if (phase == Idle) {
      if (started && area() && visible)
        prepare();
      return;
    }
    if (phase == Ready && pathPrepared && !target && started && area() && visible && !rebuild &&
        !hardRebuild) {
      rebuild = true;
      queueTick();
      return;
    }
    pump.setPaused(!state.canSubmit());
    if (!target || !state.canSubmit())
      return;
    auto stamp = pump.reserve(lr::core::ViewId{0}, state.snapshot()->generation);
    if (!stamp)
      return;
    outstanding.emplace(stamp->sequence, Record{*stamp, *target});
    auto prepareStarted = std::chrono::steady_clock::now();
    lr::render::FrameDesc frame;
    try {
      if (!pending) {
        // Push editor dirty TRS/hierarchy/mesh-mat into World before extract.
        // C1: do not catch here — outer catch completes/erases the reserved stamp.
        editor.syncToWorld(*resources.world);
        pending = resources.world->extractDelta();
        ++performance.deltaExtractions;
      }
      frame.delta = *pending;
      auto &admission = outstanding.at(stamp->sequence);
      admission.texturePayload = !frame.delta.textures.empty();
      admission.textureSettling = textureFollowupPending && !admission.texturePayload;
      if (admission.texturePayload || admission.textureSettling)
        textureFollowupPending = false;
      lr::render::ViewDesc view;
      auto current = state.snapshot();
      view.id = stamp->view;
      view.target = *target;
      view.camera = current->camera.camera;
      if (current->options.pipeline == PipelineKind::PathtraceInteractive)
        view.pipeline = resources.renderer->pipelines().pathTraceInteractive();
      else if (current->options.pipeline == PipelineKind::Pathtrace)
        view.pipeline = resources.renderer->pipelines().pathTraceReference();
      else
        view.pipeline = resources.renderer->pipelines().visibility();
      view.settings.exposure = current->options.exposure;
      view.settings.antiAliasing =
          current->options.taa ? lr::render::AntiAliasing::Taa : lr::render::AntiAliasing::None;
      view.cameraCut = current->options.taa && state.historyResetPending();
      frame.views.push_back(view);
    } catch (std::exception const &e) {
      pump.complete(*stamp);
      outstanding.erase(stamp->sequence);
      fault(issue(ErrorStage::Submit, "editor sync or pre-submit assembly failed closed", e.what()));
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
    diagnostics.recordSubmission(
        *stamp,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStarted).count());
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
    diagnostics.recordCompletion(completion);
    auto record = found->second;
    outstanding.erase(found);
    pump.complete(record.stamp);
    if (!pump.inFlight() && receiptWaitStarted) {
      receiptWaitMs +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *receiptWaitStarted)
              .count();
      receiptWaitStarted.reset();
    }
    ++performance.completed;
    if (auto e = std::get_if<RenderError>(&completion.result))
      fault(*e);
    else {
      auto result = std::get<lr::render::FrameResult>(std::move(completion.result));
      // Renderer.h ConversionDelivery::Deterministic delivers conversions in the
      // next frame, not the upload frame. Exactly one completion-driven refresh;
      // no repeated delta, timer polling, or unbounded resource retransmission.
      if (!blocked() && record.texturePayload) {
        textureFollowupPending = true;
        pump.markDirty();
      }
      if (!blocked() && record.textureSettling && record.stamp.generation == state.snapshot()->generation) {
        auto o = state.snapshot()->options;
        if (isPathTracePipeline(o.pipeline) || o.taa)
          sceneParametersChanged();
      }
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
          if (performance.accumulatedSamples == 1)
            ++performance.sampleOneFrames;
          if (result.views[0].image) {
            auto image = std::make_shared<lr::render::ImageRgba8 const>(std::move(*result.views[0].image));
            try {
              adapter->consumeImage({record.stamp, std::move(image)});
              ++performance.displayed;
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
  void sceneParametersChanged() {
    auto o = state.snapshot()->options;
    if (target && (isPathTracePipeline(o.pipeline) || o.taa)) {
      state.presentationInvalidated(
          {ErrorStage::Surface, true, "scene parameter history reset", "fresh temporal target"});
      rebuild = true;
    }
    pump.markDirty();
    queueTick();
    publish();
  }
  void historyChange(bool temporalTransition = false) {
    // A camera-only update has no temporal history in plain visibility. Keep its
    // target/epoch and acknowledge the pending reset only at accepted submit.
    auto const options = state.snapshot()->options;
    if (!temporalTransition && options.pipeline == PipelineKind::Visibility && !options.taa)
      return;
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
    pendingLoad.reset();
    readyAsset.reset();
    for (auto const &job : loads)
      job.second.cancel();
    if (installer)
      installer->rollback();
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
RenderSession::~RenderSession() noexcept {
  if (!impl_)
    return;
  if (!QCoreApplication::instance())
    qFatal("RenderSession destruction requires a live GUI application through onStopped");
  bool const guiOwner = QThread::currentThread() == impl_->thread();
  auto detachAndStop = [owner = std::move(impl_)]() noexcept {
    try {
      owner->owner();
      owner->events = {};
      owner->uiScope.reset();
      owner->stop();
    } catch (...) {
      qFatal("RenderSession noexcept GUI teardown failed");
    }
  };
  if (guiOwner) {
    detachAndStop();
  } else {
    qWarning("RenderSession: worker destruction queues observer detach and stop on GUI");
    try {
      post(std::move(detachAndStop));
    } catch (...) {
      qFatal("RenderSession could not queue noexcept GUI teardown");
    }
  }
}
void RenderSession::setPresentationMode(PresentationPath path, bool fallback) {
  auto &s = *impl_;
  s.owner();
  if (s.started)
    throw std::logic_error("presentation policy is startup-only");
  s.selectedPath = path;
  s.allowFallback = fallback;
}
void RenderSession::setVisible(bool visible) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked() || s.visible == visible)
    return;
  s.visible = visible;
  s.state.setVisible(visible);
  if (!visible)
    s.rebuild = true;
  else if (!s.target && s.pathPrepared)
    s.rebuild = true;
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::invalidatePresentation(RenderError error) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  s.state.presentationInvalidated(error);
  s.hardRebuild = true;
  s.rebuild = true;
  s.pump.markDirty();
  s.queueTick();
  s.publish();
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
  try {
    s.state.selectModel(std::move(id));
    for (auto const &job : s.loads)
      job.second.cancel();
    s.pendingLoad.reset();
    s.readyAsset.reset();
    bool wasStaging = s.installer && s.installer->active();
    s.installer->rollback();
    if (wasStaging)
      s.readoptInstalledAfterRollback(); // N3/I1: keep editor-created; rebind Installed
    if (!s.loader) {
      s.report(issue(ErrorStage::Load, "asset loading unavailable", "no AssetLoader injected"));
      return;
    }
    auto entry = AssetCatalog{}.describe(s.state.snapshot()->model.value());
    if (auto e = std::get_if<RenderError>(&entry))
      s.report(*e);
    else
      s.pendingLoad = std::get<AssetEntry>(std::move(entry));
    s.queueTick();
    s.publish();
  } catch (std::exception const &e) {
    // M2: never throw into Qt slots — fail closed.
    s.fault(issue(ErrorStage::Load, "selectModel failed closed", e.what()));
  }
}
void RenderSession::setAssetLoader(AssetLoader &loader) {
  impl_->owner();
  if (!impl_->blocked()) {
    if (!impl_->loads.empty())
      throw std::logic_error("cannot replace AssetLoader with active loads");
    impl_->loader = &loader;
  }
}
void RenderSession::setNormalMapping(bool enabled) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  try {
    s.installer->setNormalMapping(enabled);
    s.normalMapping = enabled;
    s.sceneParametersChanged();
  } catch (std::exception const &e) {
    s.fault(issue(ErrorStage::Submit, "normal material mutation failed closed", e.what()));
  }
}
void RenderSession::setAmbient(float intensity) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  if (!std::isfinite(intensity) || intensity < 0)
    throw std::invalid_argument("ambient radiance requires finite >=0");
  try {
    s.resources.world->setEnvironment({lr::core::vec3{intensity}});
    s.ambient = intensity;
    s.sceneParametersChanged();
  } catch (std::exception const &e) {
    s.fault(issue(ErrorStage::Submit, "lighting mutation failed closed", e.what()));
  }
}
void RenderSession::setRoughness(float roughness) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  if (!std::isfinite(roughness) || roughness < 0 || roughness > 1)
    throw std::invalid_argument("roughness requires finite [0,1]");
  try {
    s.installer->setRoughness(roughness);
    s.roughness = roughness;
    s.sceneParametersChanged();
  } catch (std::exception const &e) {
    s.fault(issue(ErrorStage::Submit, "material mutation failed closed", e.what()));
  }
}
void RenderSession::setCamera(CameraState camera) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  s.state.setCamera(camera);
  // Actual upstream PT continuesAccumulation compares the complete CameraDesc.
  // TAA consumes ViewDesc.cameraCut below. Both reset without retiring the
  // target/presenter/host for camera-only changes; pipeline/TAA transitions and
  // scene/extent/native loss still keep their existing drain/rebuild barrier.
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::setRenderOptions(RenderOptions options) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return;
  auto const previous = s.state.snapshot()->options;
  s.state.setRenderOptions(options);
  s.historyChange(previous.pipeline != options.pipeline || previous.taa != options.taa);
  s.pump.setContinuous(isPathTracePipeline(options.pipeline) || options.taa);
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
EditorEntityId RenderSession::createEditorEntity(std::string name, lr::core::Transform local,
                                                 std::optional<EditorEntityId> parent) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked())
    return {};
  std::optional<entt::entity> p;
  if (parent) {
    if (!parent->isValid())
      throw std::invalid_argument("createEditorEntity: invalid parent");
    auto pe = s.fromId(*parent);
    if (!s.editor.registry().valid(pe))
      throw std::invalid_argument("createEditorEntity: stale parent");
    p = pe;
  }
  auto e = s.editor.create(std::move(name), local, p);
  s.pump.markDirty();
  s.queueTick();
  s.publish();
  return s.toId(e);
}
void RenderSession::setEntityTransform(EditorEntityId id, lr::core::Transform local) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked() || !id.isValid())
    return;
  auto e = s.fromId(id);
  if (!s.editor.registry().valid(e))
    return; // N2: stale id after destroy / model switch — ignore, do not abort
  s.editor.setLocalTransform(e, local);
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::setEntityParent(EditorEntityId id, std::optional<EditorEntityId> parent) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked() || !id.isValid())
    return;
  auto e = s.fromId(id);
  if (!s.editor.registry().valid(e))
    return;
  std::optional<entt::entity> p;
  if (parent) {
    if (!parent->isValid())
      throw std::invalid_argument("setEntityParent: invalid parent");
    auto pe = s.fromId(*parent);
    if (!s.editor.registry().valid(pe))
      return;
    p = pe;
  }
  s.editor.setParent(e, p);
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::setEntityMeshMaterial(EditorEntityId id, lr::core::MeshId mesh,
                                          lr::core::MaterialId material) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked() || !id.isValid())
    return;
  auto e = s.fromId(id);
  if (!s.editor.registry().valid(e))
    return;
  s.editor.setMeshMaterial(e, mesh, material);
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
void RenderSession::destroyEntity(EditorEntityId id) {
  auto &s = *impl_;
  s.owner();
  if (s.blocked() || !id.isValid())
    return;
  auto e = s.fromId(id);
  if (!s.editor.registry().valid(e))
    return;
  s.editor.destroy(e);
  s.pump.markDirty();
  s.queueTick();
  s.publish();
}
} // namespace demo
