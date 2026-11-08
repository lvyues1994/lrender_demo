#pragma once
#include "SessionTypes.h"
#include <thread>
namespace demo {
// Pure GUI-owner state; NO World/delta storage and NO resource creation/destruction.
// Latest absolute commands coalesce here; extracted/submitted deltas remain owned
// and ordered by the future RenderSession/FramePump, never by this state container.
class SessionState {
public:
    SessionState();
    SessionState(SessionState const &) = delete;
    SessionState &operator=(SessionState const &) = delete;
    void start();
    void selectModel(ModelId model);
    void setCamera(CameraState camera);
    void setRenderOptions(RenderOptions options);
    void setViewport(ViewportExtent viewport);
    // Hiding a ready host retires its display epoch; showing waits for explicit
    // readiness/revalidation. This does not itself recreate any GPU resource.
    void setVisible(bool visible);
    void pause();
    void resume();
    void requestStop();
    void stopped(); // actual cleanup completion, NOT an automatic transition on stop request
    void fail(RenderError error);
    void presentationInvalidated(RenderError error);
    // Caller must have actually drained/rebound resources before signaling readiness.
    // This epoch check is not a substitute for adapter lifetime/flush ordering.
    // Empty backend/device values preserve known metadata during a target rebind.
    bool presentationReady(SessionGeneration generation, PresentationPath path,
                           std::string backend = {}, std::string device = {});
    SessionSnapshotPtr snapshot() const;
    ViewportExtent viewport() const;
    bool canSubmit() const;
    bool acceptsAsset(AssetRequestId request) const;
    // Epoch/display admission only: stable ViewId/sequence routing is P5/P6's job.
    // Current in-flight results may display during manual pause, never while hidden.
    // Rejected display results must STILL be accounted for by the completion owner.
    bool acceptsFrame(FrameStamp const &stamp) const;
    bool historyResetPending() const;
    // Acknowledge only after latest state was applied and submit accepted in the SAME
    // GUI turn, never before a possibly-throwing submit or from an old completion.
    // No World delta is consumed.
    // Acknowledgement is ignored while frame production is blocked.
    void acknowledgeHistoryReset();
private:
    void requireOwner() const;
    bool acceptsCommands() const;
    bool hasArea() const;
    void refreshPause();
    void retirePresentation();
    SessionSnapshot state_;
    ViewportExtent viewport_;
    std::thread::id const owner_;
    bool visible_ = true, userPaused_ = false, presentationValid_ = false;
    bool resetHistory_ = false;
};
} // namespace demo
