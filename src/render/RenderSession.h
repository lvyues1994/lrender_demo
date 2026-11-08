#pragma once
#include "SessionTypes.h"
#include "presentation/PresentationAdapter.h"
#include <lr/scene/World.h>
namespace demo {
class AssetLoader; // P8 nonowning injection only; no invented implementation
// GUI-thread lifetime and serial commands; inputs are initialized owned resources.
// Shutdown is asynchronous: keep the application event loop alive through onStopped.
// Destruction detaches UI observers, not accepted producer/cleanup work. A nonempty
// SubmitFrame override must eventually report any possibly accepted work even after
// throwing; only the real renderer path has a terminal renderer-barrier fallback.
class RenderSession {
public:
  RenderSession(std::unique_ptr<lr::runtime::Runtime>, std::unique_ptr<lr::render::Renderer>,
                std::unique_ptr<lr::scene::World>, std::unique_ptr<PresentationAdapter>, SubmitFrame,
                SessionEvents);
  ~RenderSession();
  RenderSession(RenderSession const &) = delete;
  RenderSession &operator=(RenderSession const &) = delete;
  void start();
  void selectModel(ModelId);
  void setCamera(CameraState);
  void setRenderOptions(RenderOptions);
  void setViewport(ViewportExtent);
  void pause();
  void resume();
  void requestStop();
  SessionSnapshotPtr snapshot() const;
  void setAssetLoader(AssetLoader &);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace demo
