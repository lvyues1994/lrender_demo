#include "SessionState.h"
#include <cmath>
#include <stdexcept>
namespace demo {
namespace {
bool sameViewport(ViewportExtent const &a, ViewportExtent const &b) {
    return a.logical.x == b.logical.x && a.logical.y == b.logical.y &&
           a.physical.x == b.physical.x && a.physical.y == b.physical.y && a.dpr == b.dpr;
}
bool sameVector(lr::core::vec3 const &a, lr::core::vec3 const &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
bool finiteVector(lr::core::vec3 const &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool finiteMatrix(lr::core::mat4 const &matrix) {
    for (int column = 0; column != 4; ++column)
        for (int row = 0; row != 4; ++row)
            if (!std::isfinite(matrix[column][row])) return false;
    return true;
}
bool validCamera(lr::core::CameraDesc const &camera) {
    // Scene.h specifies radians. Math.cpp uses normalize(target-position),
    // normalize(cross(forward,up)), tan(fov/2), and finite near/far products.
    // No orbit radius/pole epsilon, unit-up requirement or UI preference caps.
    if (!finiteVector(camera.position) || !finiteVector(camera.target) || !finiteVector(camera.up) ||
        !std::isfinite(camera.verticalFov) || camera.verticalFov <= 0 || camera.verticalFov >= std::acos(-1.0) ||
        !std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane) return false;
    auto const forward = camera.target - camera.position;
    auto const forwardLength2 = tc::dot(forward, forward);
    if (!std::isfinite(forwardLength2) || forwardLength2 <= 0) return false;
    auto const side = tc::cross(tc::normalize(forward), camera.up);
    auto const sideLength2 = tc::dot(side, side);
    if (!std::isfinite(sideLength2) || sideLength2 <= 0) return false;
    // Check actual float math, not guessed scene-scale limits. Aspect belongs to
    // the viewport; unit aspect verifies the camera's own projection coefficients.
    return finiteMatrix(lr::core::viewMatrix(camera)) && finiteMatrix(lr::core::projectionMatrix(camera, 1.0f));
}
bool sameCamera(CameraState const &a, CameraState const &b) {
    return sameVector(a.camera.position, b.camera.position) && sameVector(a.camera.target, b.camera.target) &&
           sameVector(a.camera.up, b.camera.up) && a.camera.verticalFov == b.camera.verticalFov &&
           a.camera.nearPlane == b.camera.nearPlane && a.camera.farPlane == b.camera.farPlane;
}
} // namespace
SessionState::SessionState() : owner_(std::this_thread::get_id()) {}
void SessionState::requireOwner() const {
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("SessionState access requires its GUI owner thread");
}
bool SessionState::acceptsCommands() const {
    return state_.life != LifeState::Stopping && state_.life != LifeState::Stopped && state_.life != LifeState::Failed;
}
bool SessionState::hasArea() const {
    return viewport_.logical.x > 0 && viewport_.logical.y > 0 &&
           viewport_.physical.x > 0 && viewport_.physical.y > 0;
}
void SessionState::retirePresentation() {
    ++state_.generation;
    presentationValid_ = false;
    resetHistory_ = true;
}
void SessionState::refreshPause() {
    if (state_.life != LifeState::Running && state_.life != LifeState::Paused) return;
    if (!visible_ || !hasArea()) state_.life = LifeState::Paused;
    else if (!presentationValid_) state_.life = LifeState::Recovering;
    else state_.life = userPaused_ ? LifeState::Paused : LifeState::Running;
}
void SessionState::start() {
    requireOwner();
    if (state_.life != LifeState::Idle) return;
    retirePresentation();
    state_.life = LifeState::Preparing;
}
void SessionState::selectModel(ModelId model) {
    requireOwner();
    if (!acceptsCommands()) return;
    state_.model = std::move(model);
    ++state_.request; // same key again is an explicit reload request, not a frame epoch
}
void SessionState::setCamera(CameraState camera) {
    requireOwner();
    if (!acceptsCommands()) return;
    if (!validCamera(camera.camera))
        throw std::invalid_argument("Camera must have finite representable lookAt/projection, 0<FOV<pi, 0<near<far");
    if (sameCamera(state_.camera, camera)) return;
    state_.camera = camera;
    resetHistory_ = true; // cameraCut/reference-history policy consumed later by session, not a target epoch
}
void SessionState::setRenderOptions(RenderOptions options) {
    requireOwner();
    if (!acceptsCommands()) return;
    if (!std::isfinite(options.exposure) || options.exposure <= 0)
        throw std::invalid_argument("Exposure must be finite and positive");
    if (state_.options.pipeline != options.pipeline || state_.options.taa != options.taa) resetHistory_ = true;
    state_.options = options; // latest absolute values coalesce; exposure alone keeps linear history
}
void SessionState::setViewport(ViewportExtent viewport) {
    requireOwner();
    if (!acceptsCommands()) return;
    if (viewport.logical.x < 0 || viewport.logical.y < 0 || viewport.physical.x < 0 || viewport.physical.y < 0 ||
        !std::isfinite(viewport.dpr) || viewport.dpr <= 0)
        throw std::invalid_argument("Viewport extents must be nonnegative and DPR finite/positive");
    if (sameViewport(viewport_, viewport)) return;
    viewport_ = viewport; // caller supplies actual physical pixels; no rounding policy hidden here
    retirePresentation();
    refreshPause(); // positive resize waits for actual target/readiness acknowledgement
}
void SessionState::setVisible(bool visible) {
    requireOwner();
    if (!acceptsCommands() || visible_ == visible) return;
    visible_ = visible;
    if (!visible) retirePresentation(); // also invalidate queued readiness from Preparing/Recovering
    refreshPause();
}
void SessionState::pause() {
    requireOwner();
    if (!acceptsCommands()) return;
    userPaused_ = true;
    refreshPause();
}
void SessionState::resume() {
    requireOwner();
    if (!acceptsCommands()) return;
    userPaused_ = false;
    refreshPause();
}
void SessionState::requestStop() {
    requireOwner();
    if (state_.life == LifeState::Stopping || state_.life == LifeState::Stopped) return;
    retirePresentation();
    state_.life = LifeState::Stopping;
}
void SessionState::stopped() {
    requireOwner();
    if (state_.life == LifeState::Stopping) state_.life = LifeState::Stopped;
}
void SessionState::fail(RenderError error) {
    requireOwner();
    if (!acceptsCommands()) return;
    state_.error = std::move(error);
    retirePresentation();
    state_.life = LifeState::Failed;
}
void SessionState::presentationInvalidated(RenderError error) {
    requireOwner();
    if (!acceptsCommands() || state_.life == LifeState::Idle) return;
    auto const recoverable = error.recoverable;
    state_.error = std::move(error);
    retirePresentation();
    state_.life = recoverable ? LifeState::Recovering : LifeState::Failed;
}
bool SessionState::presentationReady(SessionGeneration generation, PresentationPath path,
                                     std::string backend, std::string device) {
    requireOwner();
    if (!acceptsCommands() || !visible_ || state_.life == LifeState::Idle || presentationValid_ ||
        generation == 0 || generation != state_.generation) return false;
    presentationValid_ = true;
    state_.path = path;
    if (!backend.empty()) state_.backend = std::move(backend);
    if (!device.empty()) state_.device = std::move(device);
    // Retain the last reported cause after successful fallback/recovery, not an
    // invented failure of the new path. LifeState determines whether it is active.
    state_.life = userPaused_ || !visible_ || !hasArea() ? LifeState::Paused : LifeState::Running;
    resetHistory_ = true;
    return true;
}
SessionSnapshotPtr SessionState::snapshot() const {
    requireOwner();
    return std::make_shared<SessionSnapshot const>(state_);
}
ViewportExtent SessionState::viewport() const { requireOwner(); return viewport_; }
bool SessionState::canSubmit() const {
    requireOwner();
    return state_.life == LifeState::Running && presentationValid_ && visible_ && !userPaused_ && hasArea();
}
bool SessionState::acceptsAsset(AssetRequestId request) const {
    requireOwner();
    return acceptsCommands() && request != 0 && request == state_.request;
}
bool SessionState::acceptsFrame(FrameStamp const &stamp) const {
    requireOwner();
    return (state_.life == LifeState::Running || state_.life == LifeState::Paused) &&
           presentationValid_ && visible_ && hasArea() && stamp.generation != 0 && stamp.generation == state_.generation;
}
bool SessionState::historyResetPending() const { requireOwner(); return resetHistory_; }
void SessionState::acknowledgeHistoryReset() {
    requireOwner();
    if (canSubmit()) resetHistory_ = false;
}
} // namespace demo
