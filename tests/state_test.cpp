#include "render/SessionState.h"
#include <doctest/doctest.h>
#include <future>
#include <limits>
#include <stdexcept>
#include <type_traits>
using namespace demo;
namespace {
ViewportExtent extent(int width = 100, int height = 80, double dpr = 1) {
    return {{width, height}, {int(width * dpr), int(height * dpr)}, dpr};
}
void ready(SessionState &state) {
    state.setViewport(extent()); state.start();
    REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::VulkanShared, "vulkan", "test-device"));
}
FrameStamp stamp(SessionState const &state) { return {lr::core::ViewId{7}, state.snapshot()->generation, 1}; }
}
TEST_CASE("state/request-and-snapshot") {
    SessionState state;
    auto before = state.snapshot();
    state.selectModel({"african_head"});
    auto after = state.snapshot();
    CHECK(after->request == before->request + 1);
    CHECK_FALSE(before->model.has_value());
    REQUIRE(after->model.has_value());
    CHECK(after->model->value == "african_head");
    state.requestStop(); auto stopped = state.snapshot();
    state.selectModel({"boggie"});
    CHECK(state.snapshot()->request == stopped->request);
}
TEST_CASE("state/start-ready-pause-resume-stop") {
    SessionState state;
    CHECK(state.snapshot()->life == LifeState::Idle);
    CHECK_FALSE(state.canSubmit());
    state.setViewport(extent()); state.start();
    CHECK(state.snapshot()->life == LifeState::Preparing);
    CHECK_FALSE(state.canSubmit());
    auto generation = state.snapshot()->generation;
    state.start(); CHECK(state.snapshot()->generation == generation);
    REQUIRE(state.presentationReady(generation, PresentationPath::VulkanShared, "vulkan", "device"));
    CHECK(state.canSubmit());
    CHECK(state.snapshot()->backend == "vulkan");
    state.pause(); CHECK(state.snapshot()->life == LifeState::Paused); CHECK_FALSE(state.canSubmit());
    CHECK(state.acceptsFrame(stamp(state))); // finish/display a current frame while manually paused
    state.resume(); CHECK(state.canSubmit());
    state.requestStop(); generation = state.snapshot()->generation;
    CHECK(state.snapshot()->life == LifeState::Stopping);
    CHECK_FALSE(state.canSubmit());
    state.requestStop(); CHECK(state.snapshot()->generation == generation);
    state.stopped(); CHECK(state.snapshot()->life == LifeState::Stopped);
    state.resume(); state.start(); CHECK_FALSE(state.canSubmit());
}
TEST_CASE("state/zero-physical-extent-really-blocks-submission") {
    SessionState state; ready(state); auto old = stamp(state);
    auto zero = extent(); zero.physical.x = 0;
    state.setViewport(zero);
    CHECK(state.snapshot()->life == LifeState::Paused);
    CHECK_FALSE(state.canSubmit()); CHECK_FALSE(state.acceptsFrame(old));
    auto generation = state.snapshot()->generation;
    REQUIRE(state.presentationReady(generation, PresentationPath::VulkanShared));
    CHECK_FALSE(state.canSubmit()); // even a ready target cannot override zero area
    state.resume(); CHECK_FALSE(state.canSubmit());
    state.setViewport(extent()); CHECK_FALSE(state.canSubmit());
    REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::VulkanShared));
    CHECK(state.canSubmit());
}
TEST_CASE("state/hidden-host-retires-old-display-and-awaits-revalidation") {
    SessionState state; ready(state); auto old = stamp(state);
    state.setVisible(false);
    CHECK_FALSE(state.canSubmit()); CHECK_FALSE(state.acceptsFrame(old));
    auto generation = state.snapshot()->generation;
    state.setVisible(false); CHECK(state.snapshot()->generation == generation);
    state.setVisible(true); CHECK_FALSE(state.canSubmit());
    CHECK_FALSE(state.presentationReady(old.generation, PresentationPath::VulkanShared));
    REQUIRE(state.presentationReady(generation, PresentationPath::VulkanShared));
    CHECK(state.canSubmit()); CHECK_FALSE(state.acceptsFrame(old));
}
TEST_CASE("state/manual-pause-survives-hide-resize-and-recovery") {
    SessionState state; ready(state); state.pause();
    state.setVisible(false); state.setViewport(extent(120, 90)); state.setVisible(true);
    REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::VulkanShared));
    CHECK(state.snapshot()->life == LifeState::Paused); CHECK_FALSE(state.canSubmit());
    state.resume(); CHECK(state.canSubmit());
}
TEST_CASE("state/viewport-generation-dpr-and-idempotence") {
    SessionState state; ready(state); auto old = stamp(state);
    state.setViewport(extent(120, 90, 2));
    CHECK(state.viewport().physical.x == 240);
    CHECK(state.viewport().dpr == 2);
    CHECK(state.snapshot()->generation == old.generation + 1);
    CHECK_FALSE(state.canSubmit());
    CHECK_FALSE(state.presentationReady(old.generation, PresentationPath::Readback));
    auto generation = state.snapshot()->generation;
    state.setViewport(extent(120, 90, 2)); CHECK(state.snapshot()->generation == generation);
    REQUIRE(state.presentationReady(generation, PresentationPath::VulkanShared));
    CHECK(state.canSubmit()); CHECK_FALSE(state.acceptsFrame(old));
    // A fractional DPR change can legitimately keep rounded physical pixels equal.
    state.setViewport({{1, 1}, {1, 1}, 1});
    generation = state.snapshot()->generation;
    auto dprOnly = state.viewport(); dprOnly.dpr = 1.25;
    state.setViewport(dprOnly); CHECK(state.snapshot()->generation == generation + 1);
}
TEST_CASE("state/latest-camera-options-coalesce-with-latched-history-not-frame-epochs") {
    SessionState state; ready(state); state.acknowledgeHistoryReset();
    auto generation = state.snapshot()->generation;
    CameraState camera; camera.camera.position.x = 1;
    state.setCamera(camera); camera.camera.position.x = 2; state.setCamera(camera);
    RenderOptions options; options.pipeline = PipelineKind::Pathtrace; options.exposure = 2;
    state.setRenderOptions(options); options.exposure = 3; state.setRenderOptions(options);
    CHECK(state.snapshot()->camera.camera.position.x == 2);
    CHECK(state.snapshot()->options.exposure == 3);
    CHECK(state.snapshot()->options.pipeline == PipelineKind::Pathtrace);
    CHECK(state.snapshot()->generation == generation);
    CHECK(state.historyResetPending());
    state.acknowledgeHistoryReset(); CHECK_FALSE(state.historyResetPending());
    state.setCamera(camera); state.setRenderOptions(options); CHECK_FALSE(state.historyResetPending());
    options.exposure = 4; state.setRenderOptions(options); CHECK_FALSE(state.historyResetPending());
    options.taa = true; state.setRenderOptions(options); CHECK(state.historyResetPending());
}
TEST_CASE("state/asset-request-is-independent-and-old-results-are-rejected") {
    SessionState state; ready(state);
    auto generation = state.snapshot()->generation;
    CHECK_FALSE(state.acceptsAsset(0));
    state.selectModel({"first"}); auto first = state.snapshot()->request;
    state.selectModel({"second"}); auto second = state.snapshot()->request;
    CHECK_FALSE(state.acceptsAsset(first)); CHECK(state.acceptsAsset(second));
    CHECK(state.snapshot()->generation == generation);
    state.setViewport(extent(200, 80)); CHECK(state.acceptsAsset(second));
    state.requestStop(); CHECK_FALSE(state.acceptsAsset(second));
}
TEST_CASE("state/recovering-and-failed-cannot-be-resumed-by-stale-ready") {
    SessionState state; ready(state); auto old = stamp(state);
    state.presentationInvalidated({ErrorStage::Surface, true, "native host lost", "old XID"});
    CHECK(state.snapshot()->life == LifeState::Recovering); CHECK_FALSE(state.canSubmit());
    CHECK_FALSE(state.acceptsFrame(old));
    state.resume(); CHECK_FALSE(state.canSubmit());
    CHECK_FALSE(state.presentationReady(old.generation, PresentationPath::VulkanShared));
    REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::Readback, "vulkan", "same-device"));
    CHECK(state.canSubmit()); CHECK(state.snapshot()->path == PresentationPath::Readback);
    CHECK(state.snapshot()->backend == "vulkan");
    state.fail({ErrorStage::Submit, false, "device failed", "original cause"});
    CHECK(state.snapshot()->life == LifeState::Failed); CHECK_FALSE(state.canSubmit());
    REQUIRE(state.snapshot()->error.has_value()); CHECK(state.snapshot()->error->cause == "original cause");
    state.resume(); state.start();
    CHECK_FALSE(state.presentationReady(state.snapshot()->generation, PresentationPath::Readback));
    CHECK(state.snapshot()->life == LifeState::Failed);
    state.requestStop(); state.stopped(); CHECK(state.snapshot()->life == LifeState::Stopped);
}
TEST_CASE("state/close-rejects-all-new-command-and-async-ready") {
    SessionState state; ready(state); state.selectModel({"original"}); state.requestStop();
    auto before = state.snapshot(); auto viewport = state.viewport();
    state.selectModel({"ignored"}); state.setCamera({}); state.setRenderOptions({});
    state.setViewport(extent(999, 999)); state.setVisible(false); state.pause(); state.resume(); state.start();
    state.presentationInvalidated({ErrorStage::Surface, true, "late", "cause"});
    state.fail({ErrorStage::Load, false, "late", "cause"});
    CHECK_FALSE(state.presentationReady(before->generation, PresentationPath::Readback, "changed", "changed"));
    auto after = state.snapshot();
    CHECK(after->life == LifeState::Stopping); CHECK(after->request == before->request);
    CHECK(after->generation == before->generation); CHECK(after->model->value == "original");
    CHECK(after->backend == before->backend); CHECK(state.viewport().physical.x == viewport.physical.x);
    CHECK_FALSE(state.acceptsAsset(before->request)); CHECK_FALSE(state.acceptsFrame(stamp(state)));
}
TEST_CASE("state/snapshot-is-deep-immutable-value-not-world-access") {
    static_assert(std::is_same<SessionSnapshotPtr::element_type, SessionSnapshot const>::value, "immutable snapshot");
    SessionState state; ready(state); state.selectModel({"old"}); auto before = state.snapshot();
    state.selectModel({"new"}); CameraState camera; camera.camera.position.x = 99; state.setCamera(camera);
    state.fail({ErrorStage::Present, false, "new error", "new cause"});
    CHECK(before->model->value == "old"); CHECK(before->camera.camera.position.x != 99);
    CHECK(before->life == LifeState::Running); CHECK_FALSE(before->error.has_value());
    CHECK_FALSE(before->performance.gpuPassTotalMs.has_value());
    CHECK(before->performance.inFlight == 0);
}
TEST_CASE("state/invalid-viewport-is-atomic-and-does-not-change-epoch") {
    SessionState state; ready(state); auto before = state.snapshot();
    auto bad = extent(); bad.physical.x = -1; CHECK_THROWS_AS(state.setViewport(bad), std::invalid_argument);
    bad = extent(); bad.dpr = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(state.setViewport(bad), std::invalid_argument);
    bad = extent(); bad.dpr = 0; CHECK_THROWS_AS(state.setViewport(bad), std::invalid_argument);
    CHECK(state.snapshot()->generation == before->generation); CHECK(state.canSubmit());
}
TEST_CASE("state/only-owner-thread-accesses-mutable-state") {
    SessionState state; ready(state); auto before = state.snapshot();
    auto rejected = std::async(std::launch::async, [&] {
        int failures = 0;
        try { state.selectModel({"worker"}); } catch (std::logic_error const &) { ++failures; }
        try { (void)state.snapshot(); } catch (std::logic_error const &) { ++failures; }
        return failures;
    });
    CHECK(rejected.get() == 2);
    CHECK(state.snapshot()->request == before->request);
}

TEST_CASE("state/history-reset-is-not-acknowledged-while-production-is-blocked") {
    SessionState state; ready(state); state.acknowledgeHistoryReset();
    state.pause(); CameraState camera; camera.camera.target.x = 2; state.setCamera(camera);
    state.acknowledgeHistoryReset(); CHECK(state.historyResetPending());
    state.resume(); state.acknowledgeHistoryReset(); CHECK_FALSE(state.historyResetPending());
    state.presentationInvalidated({ErrorStage::Surface, true, "lost", "native event"});
    state.acknowledgeHistoryReset(); CHECK(state.historyResetPending());
}
TEST_CASE("state/rebind-preserves-known-device-metadata-and-ignores-duplicate-ready") {
    SessionState state; ready(state); state.setViewport(extent(120, 90));
    auto generation = state.snapshot()->generation;
    REQUIRE(state.presentationReady(generation, PresentationPath::VulkanShared));
    CHECK(state.snapshot()->backend == "vulkan");
    CHECK(state.snapshot()->device == "test-device");
    CHECK_FALSE(state.presentationReady(generation, PresentationPath::Readback, "cpu", "stale duplicate"));
    CHECK(state.snapshot()->path == PresentationPath::VulkanShared);
    CHECK(state.snapshot()->backend == "vulkan");
}

TEST_CASE("state/prestart-pause-and-cleanup-completion-are-not-shortcuts") {
    SessionState state; state.pause(); state.stopped();
    CHECK(state.snapshot()->life == LifeState::Idle);
    ready(state); CHECK_FALSE(state.canSubmit());
    CHECK(state.snapshot()->life == LifeState::Paused);
    state.resume(); CHECK(state.canSubmit());
    state.stopped(); CHECK(state.snapshot()->life == LifeState::Running);
    state.presentationInvalidated({ErrorStage::Surface, false, "native failure", "original native cause"});
    CHECK(state.snapshot()->life == LifeState::Failed);
    state.resume(); CHECK_FALSE(state.canSubmit());
    state.requestStop();
    REQUIRE(state.snapshot()->error.has_value()); CHECK(state.snapshot()->error->cause == "original native cause");
    state.stopped(); CHECK(state.snapshot()->life == LifeState::Stopped);
}
TEST_CASE("state/captured-immutable-snapshot-may-be-read-on-worker") {
    SessionState state; ready(state); state.selectModel({"captured"});
    auto captured = state.snapshot();
    auto result = std::async(std::launch::async, [captured] { return captured->model->value; });
    state.selectModel({"later"});
    CHECK(result.get() == "captured");
    CHECK(state.snapshot()->model->value == "later");
}
