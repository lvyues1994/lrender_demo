#pragma once
#include "SessionTypes.h"
#include "presentation/PresentationAdapter.h"
#include <lr/scene/World.h>
namespace demo {
class AssetLoader; // P8 nonowning injection only; no invented implementation
// Construct and issue serial commands on GUI; inputs are initialized owned resources.
// Supply a serial-host World (no parallel-propagation waits on the GUI owner).
// Textured dirty-only rendering requires Deterministic texture conversions and
// a streaming budget fitting the bounded catalog batch (production main config).
// A completion-driven texture follow-up settles conversions; temporal history is
// retired once more after that receipt, never by resending extracted resources.
// Shutdown is asynchronous: keep the application event loop alive through onStopped.
// Destruction is noexcept. Off-owner destruction queues GUI detach/stop and diagnoses;
// callback targets must stay valid until queued detach. No off-owner World/scope access.
// A missing application/failed teardown is a fatal contract diagnostic, not an exception.
// Destruction detaches UI observers, not accepted producer/cleanup work. A nonempty
// SubmitFrame override must eventually report any possibly accepted work even after
// throwing; only the real renderer path has a terminal renderer-barrier fallback.
//
// E2 editor: value commands only (no mutable EditorScene&). entt lives in the pimpl.
class RenderSession {
public:
  RenderSession(std::unique_ptr<lr::runtime::Runtime>, std::unique_ptr<lr::render::Renderer>,
                std::unique_ptr<lr::scene::World>, std::unique_ptr<PresentationAdapter>, SubmitFrame,
                SessionEvents);
  ~RenderSession() noexcept;
  RenderSession(RenderSession const &) = delete;
  RenderSession &operator=(RenderSession const &) = delete;
  void setPresentationMode(PresentationPath, bool allowFallback);
  void setVisible(bool);
  void invalidatePresentation(RenderError);
  void start();
  void selectModel(ModelId);
  void setNormalMapping(bool);
  void setAmbient(float);
  void setRoughness(float);
  void setCamera(CameraState);
  void setRenderOptions(RenderOptions);
  void setViewport(ViewportExtent);
  void pause();
  void resume();
  void requestStop();
  SessionSnapshotPtr snapshot() const;
  // Nonowning: ApplicationLifetime keeps the loader alive through session drain.
  void setAssetLoader(AssetLoader &);

  // --- E2 editor value commands (GUI thread) ---
  EditorEntityId createEditorEntity(std::string name, lr::core::Transform local = {},
                                    std::optional<EditorEntityId> parent = std::nullopt);
  void setEntityTransform(EditorEntityId, lr::core::Transform);
  void setEntityParent(EditorEntityId, std::optional<EditorEntityId> parent);
  void setEntityMeshMaterial(EditorEntityId, lr::core::MeshId, lr::core::MaterialId);
  void setEntityMaterialDesc(EditorEntityId, lr::core::MaterialDesc);
  void clearEntityMaterialDesc(EditorEntityId);
  void setEntityLight(EditorEntityId, lr::core::LightDesc);
  void clearEntityLight(EditorEntityId);
  void setEntityCamera(EditorEntityId, lr::scene::CameraLens);
  void clearEntityCamera(EditorEntityId);
  void destroyEntity(EditorEntityId);

  // --- E3 Undo/Redo (TRS + Mesh/Material refs only; GUI thread) ---
  // Custom CommandHistory (not QUndoStack): domain testable without Widgets,
  // beginMerge/endMerge coalesce for E5 gizmo, stale ids skip safely.
  void undo();
  void redo();
  bool canUndo() const;
  bool canRedo() const;
  void beginEditMerge(); // E5 gizmo drag / continuous slider
  void endEditMerge();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace demo
