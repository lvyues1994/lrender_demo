#include "render/Diagnostics.h"
#include <doctest/doctest.h>
#include <limits>
using namespace demo;
namespace {
FrameStamp stamp(unsigned n) { return {lr::core::ViewId{0}, 1, n}; }
FrameCompletion frame(unsigned n) {
  lr::render::FrameResult r;
  r.wallTime = std::chrono::milliseconds(23);
  r.views.resize(1);
  return {stamp(n), r};
}
} // namespace
TEST_CASE("diagnostics/host-and-device-time-never-mixed") {
  Diagnostics d(true);
  d.recordSubmission(stamp(1), 7);
  auto f = frame(1);
  d.recordCompletion(f);
  auto s = d.snapshot();
  CHECK(s.completed == 1);
  CHECK(s.inFlight == 0);
  CHECK(s.hostPrepareMs == 7);
  CHECK(s.frameWallMs == 23);
  CHECK_FALSE(s.gpuPassTotalMs);
}
TEST_CASE("diagnostics/gpu-only-complete-valid-pass-set-not-cpu-or-partial") {
  Diagnostics d(true);
  auto f = frame(1);
  auto &r = std::get<lr::render::FrameResult>(f.result);
  r.views[0].passTimings = {{"a", 1, 2.0}, {"b", 1, 3.0}};
  d.recordSubmission(stamp(1), 1);
  d.recordCompletion(f);
  CHECK(d.snapshot().gpuPassTotalMs == 5);
  f.stamp = stamp(2);
  r.views[0].passTimings[1].milliseconds.reset();
  d.recordSubmission(stamp(2), 1);
  d.recordCompletion(f);
  CHECK_FALSE(d.snapshot().gpuPassTotalMs);
  f.stamp = stamp(3);
  r.views[0].passTimings[1].milliseconds = std::numeric_limits<double>::quiet_NaN();
  d.recordSubmission(stamp(3), 1);
  d.recordCompletion(f);
  CHECK_FALSE(d.snapshot().gpuPassTotalMs);
  Diagnostics cpu;
  f.stamp = stamp(4);
  r.views[0].passTimings[1].milliseconds = 3;
  cpu.recordSubmission(stamp(4), 1);
  cpu.recordCompletion(f);
  CHECK_FALSE(cpu.snapshot().gpuPassTotalMs);
}
TEST_CASE("diagnostics/window-correlation-display-and-failure") {
  auto t = Diagnostics::Clock::time_point{};
  Diagnostics d(false, [&] { return t; });
  d.recordSubmission(stamp(1), 1);
  d.recordSubmission(stamp(1), 9);
  d.recordSubmission(stamp(2), 2);
  CHECK(d.snapshot().inFlight == 2);
  auto wrong = frame(1);
  wrong.stamp.generation = 2;
  d.recordCompletion(wrong);
  CHECK(d.snapshot().inFlight == 2);
  t += std::chrono::milliseconds(500);
  d.recordCompletion(frame(1));
  d.recordCompletion(frame(1));
  auto s = d.snapshot();
  CHECK(s.completed == 1);
  CHECK(s.inFlight == 1);
  CHECK(s.displayed == 0);
  CHECK(s.completionFps == doctest::Approx(2));
  d.recordPresentation(9, 3, 4);
  CHECK(d.snapshot().displayed == 9);
  CHECK(d.snapshot().dropped == 3);
  CHECK(d.snapshot().presenterLatencyMs == 4);
  FrameCompletion failed{stamp(2), RenderError{ErrorStage::Submit, false, "fail", "cause"}};
  d.recordCompletion(failed);
  CHECK(d.snapshot().inFlight == 0);
  CHECK(d.snapshot().completed == 2);
  CHECK_FALSE(d.snapshot().frameWallMs);
  t += std::chrono::seconds(2);
  CHECK(d.snapshot().completionFps == 0);
  CHECK_THROWS_AS(d.recordSubmission(stamp(3), -1), std::invalid_argument);
}
TEST_CASE("diagnostics/no-fake-gpu-time") {
  Diagnostics d;
  d.recordCompletion({{}, lr::render::FrameResult{}});
  CHECK_FALSE(d.snapshot().gpuPassTotalMs);
}

TEST_CASE("diagnostics/no-presentation-samples-means-unavailable-latency") {
  Diagnostics d;
  d.recordPresentation(0, 0, 0.0);
  CHECK_FALSE(d.snapshot().presenterLatencyMs);
  d.recordPresentation(1, 0, 3.0);
  CHECK(d.snapshot().presenterLatencyMs == 3);
  d.recordPresentation(1, 0, std::numeric_limits<double>::infinity());
  CHECK_FALSE(d.snapshot().presenterLatencyMs);
}
