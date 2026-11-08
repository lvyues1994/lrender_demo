#pragma once
#include "SessionTypes.h"
#include <chrono>
#include <deque>
#include <map>
namespace demo {
// GUI/serial-owner accumulator, not a background monitoring service.
// Frame pass times are GPU-labelled only with caller-supplied real GPU identity.
class Diagnostics {
public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  explicit Diagnostics(bool gpuDevice = false, Now now = [] { return Clock::now(); });
  void recordSubmission(FrameStamp, double hostPrepareMs);
  void recordCompletion(FrameCompletion const &);
  void recordPresentation(std::uint64_t shown, std::uint64_t dropped, std::optional<double> latencyMs);
  PerformanceSnapshot snapshot() const;

private:
  bool gpu_;
  Now now_;
  Clock::time_point started_;
  PerformanceSnapshot value_;
  std::map<std::uint64_t, FrameStamp> pending_;
  std::deque<Clock::time_point> completed_;
};
} // namespace demo
