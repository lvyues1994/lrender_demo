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


TEST_CASE("state/fix-hidden-ready-is-rejected-and-show-can-revalidate") {
    for (bool recovering : {false, true}) {
        INFO("recovering=", recovering);
        SessionState state; state.setViewport(extent()); state.start();
        if (recovering) {
            REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::VulkanShared));
            state.presentationInvalidated({ErrorStage::Surface, true, "lost", "event"});
        }
        auto generation = state.snapshot()->generation;
        state.setVisible(false);
        auto before = state.snapshot();
        CHECK_FALSE(state.presentationReady(generation, PresentationPath::VulkanShared, "hidden", "hidden"));
        CHECK(state.snapshot()->life == before->life);
        CHECK(state.snapshot()->backend == before->backend);
        CHECK(state.snapshot()->device == before->device);
        CHECK_FALSE(state.canSubmit());
        state.setVisible(true);
        CHECK_FALSE(state.canSubmit());
        // Explicit post-show confirmation of the still-current initialization/recovery
        // may succeed: rejecting a hidden callback must not permanently strand init.
        REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::VulkanShared, "vulkan", "shown"));
        CHECK(state.canSubmit());
        CHECK(state.snapshot()->device == "shown");
    }
}
TEST_CASE("state/fix-invalid-camera-is-atomic") {
    auto const nan = std::numeric_limits<float>::quiet_NaN();
    auto const inf = std::numeric_limits<float>::infinity();
    std::vector<CameraState> invalid;
    for (auto member : {&lr::core::CameraDesc::position, &lr::core::CameraDesc::target, &lr::core::CameraDesc::up})
        for (int axis = 0; axis != 3; ++axis)
            for (float value : {nan, inf, -inf}) {
                CameraState camera; (camera.camera.*member)[axis] = value; invalid.push_back(camera);
            }
    for (auto member : {&lr::core::CameraDesc::verticalFov, &lr::core::CameraDesc::nearPlane, &lr::core::CameraDesc::farPlane})
        for (float value : {nan, inf, -inf}) {
            CameraState camera; camera.camera.*member = value; invalid.push_back(camera);
        }
    for (float fov : {0.0f, -0.1f, lr::core::kPi, 2 * lr::core::kPi, std::numeric_limits<float>::denorm_min()}) {
        CameraState camera; camera.camera.verticalFov = fov; invalid.push_back(camera);
    }
    for (float near : {0.0f, -1.0f, 500.0f, 501.0f}) {
        CameraState camera; camera.camera.nearPlane = near; invalid.push_back(camera);
    }
    for (float far : {-1.0f, 0.0f, 0.05f}) {
        CameraState camera; camera.camera.farPlane = far; invalid.push_back(camera);
    }
    CameraState coincident; coincident.camera.target = coincident.camera.position; invalid.push_back(coincident);
    for (auto up : {lr::core::vec3{0}, lr::core::vec3{0,0,1}, lr::core::vec3{0,0,-1}}) {
        CameraState camera; camera.camera.up = up; invalid.push_back(camera);
    }
    CameraState overflow; overflow.camera.nearPlane = std::numeric_limits<float>::max()/2;
    overflow.camera.farPlane = std::numeric_limits<float>::max(); invalid.push_back(overflow);
    for (std::size_t i = 0; i != invalid.size(); ++i) {
        INFO("invalid camera case ", i);
        SessionState state; ready(state); state.acknowledgeHistoryReset();
        auto before = state.snapshot();
        CHECK_THROWS_AS(state.setCamera(invalid[i]), std::invalid_argument);
        auto after = state.snapshot();
        for (auto member : {&lr::core::CameraDesc::position, &lr::core::CameraDesc::target, &lr::core::CameraDesc::up})
            for (int axis = 0; axis != 3; ++axis)
                CHECK((after->camera.camera.*member)[axis] == (before->camera.camera.*member)[axis]);
        CHECK(after->camera.camera.verticalFov == before->camera.camera.verticalFov);
        CHECK(after->camera.camera.nearPlane == before->camera.camera.nearPlane);
        CHECK(after->camera.camera.farPlane == before->camera.camera.farPlane);
        CHECK(after->generation == before->generation);
        CHECK(after->life == before->life);
        CHECK_FALSE(state.historyResetPending()); CHECK(state.canSubmit());
    }
}
TEST_CASE("state/fix-invalid-exposure-is-atomic-even-with-pipeline-and-taa-change") {
    for (float exposure : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), 0.0f, -1.0f}) {
        SessionState state; ready(state); state.acknowledgeHistoryReset(); auto before = state.snapshot();
        RenderOptions bad; bad.pipeline = PipelineKind::Pathtrace; bad.taa = true; bad.exposure = exposure;
        CHECK_THROWS_AS(state.setRenderOptions(bad), std::invalid_argument);
        auto after = state.snapshot();
        CHECK(after->options.pipeline == before->options.pipeline);
        CHECK(after->options.taa == before->options.taa);
        CHECK(after->options.exposure == before->options.exposure);
        CHECK(after->generation == before->generation); CHECK(after->life == before->life);
        CHECK_FALSE(state.historyResetPending()); CHECK(state.canSubmit());
    }
}
TEST_CASE("state/fix-valid-camera-and-exposure-have-no-ui-preference-caps") {
    SessionState state; ready(state);
    CameraState camera; camera.camera.up = lr::core::vec3{0,3,1}; // nonunit/nonorthogonal is a valid lookAt hint
    camera.camera.verticalFov = 3.14f; camera.camera.farPlane = 1e20f;
    CHECK_NOTHROW(state.setCamera(camera));
    CHECK(state.snapshot()->camera.camera.farPlane == 1e20f);
    RenderOptions options; options.exposure = std::numeric_limits<float>::max();
    CHECK_NOTHROW(state.setRenderOptions(options));
    options.exposure = std::numeric_limits<float>::min();
    CHECK_NOTHROW(state.setRenderOptions(options));
    CHECK(state.snapshot()->options.exposure == options.exposure);
}
TEST_CASE("state/fix-preparing-logical-only-viewport-change-retires-ready") {
    SessionState state; state.setViewport(extent()); state.start(); auto generation = state.snapshot()->generation;
    auto changed = state.viewport(); ++changed.logical.x; state.setViewport(changed);
    CHECK(state.snapshot()->life == LifeState::Preparing);
    CHECK(state.snapshot()->generation == generation + 1);
    CHECK(state.viewport().physical.x == 100); CHECK(state.viewport().dpr == 1);
    CHECK_FALSE(state.presentationReady(generation, PresentationPath::VulkanShared));
    CHECK_FALSE(state.canSubmit());
    REQUIRE(state.presentationReady(state.snapshot()->generation, PresentationPath::VulkanShared));
    CHECK(state.canSubmit());
}
TEST_CASE("state/fix-recovering-and-failed-reject-current-epoch-frames-too") {
    SessionState state; ready(state);
    state.presentationInvalidated({ErrorStage::Surface, true, "lost", "event"});
    CHECK_FALSE(state.acceptsFrame(stamp(state)));
    state.fail({ErrorStage::Submit, false, "fatal", "cause"});
    CHECK_FALSE(state.acceptsFrame(stamp(state)));
}

TEST_CASE("state/p4-queued-hidden-ready-across-show-is-stale") {
 for (bool recovery : {false,true}) {
  SessionState s; s.selectModel({"asset"}); s.setViewport(extent()); s.start();
  if(recovery) s.presentationInvalidated({ErrorStage::Surface,true,"lost","native"});
  auto generation=s.snapshot()->generation; auto request=s.snapshot()->request;
  s.setVisible(false); s.setVisible(true);
  CHECK(s.snapshot()->generation == generation+1); CHECK(s.snapshot()->request==request);
  CHECK_FALSE(s.presentationReady(generation,PresentationPath::VulkanShared));
  CHECK_FALSE(s.canSubmit());
  REQUIRE(s.presentationReady(s.snapshot()->generation,PresentationPath::VulkanShared)); CHECK(s.canSubmit());
 }
}
