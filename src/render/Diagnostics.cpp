#include "Diagnostics.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace demo {
namespace {
bool valid(double v) { return std::isfinite(v) && v >= 0; }
} // namespace
Diagnostics::Diagnostics(bool g, Now n) : gpu_(g), now_(std::move(n)), started_(now_()) {}
void Diagnostics::recordSubmission(FrameStamp stamp, double ms) {
  if (!valid(ms))
    throw std::invalid_argument("host prepare must be finite >=0");
  if (pending_.emplace(stamp.sequence, stamp).second)
    value_.hostPrepareMs = ms;
}
void Diagnostics::recordCompletion(FrameCompletion const &f) {
  auto p = pending_.find(f.stamp.sequence);
  if (p == pending_.end() || p->second.generation != f.stamp.generation || p->second.view != f.stamp.view)
    return;
  pending_.erase(p);
  ++value_.completed;
  value_.frameWallMs.reset();
  value_.gpuPassTotalMs.reset();
  auto now = now_();
  completed_.push_back(now);
  while (!completed_.empty() && now - completed_.front() > std::chrono::seconds(1))
    completed_.pop_front();
  auto r = std::get_if<lr::render::FrameResult>(&f.result);
  if (!r)
    return;
  if (valid(r->wallTime.count()))
    value_.frameWallMs = r->wallTime.count();
  if (!gpu_ || r->views.empty())
    return;
  double total = 0;
  for (auto const &v : r->views) {
    if (v.passTimings.empty())
      return;
    for (auto const &t : v.passTimings) {
      if (!t.milliseconds || !valid(*t.milliseconds))
        return;
      total += *t.milliseconds;
      if (!valid(total))
        return;
    }
  }
  value_.gpuPassTotalMs = total;
}
void Diagnostics::recordPresentation(std::uint64_t shown, std::uint64_t dropped,
                                     std::optional<double> latency) {
  value_.displayed = shown;
  value_.dropped = dropped;
  value_.presenterLatencyMs = shown && latency && valid(*latency) ? latency : std::nullopt;
}
PerformanceSnapshot Diagnostics::snapshot() const {
  auto s = value_;
  s.inFlight = static_cast<std::uint32_t>(pending_.size());
  auto now = now_();
  s.windowSeconds = std::clamp(std::chrono::duration<double>(now - started_).count(), 0., 1.);
  auto n = std::count_if(completed_.begin(), completed_.end(),
                         [&](auto t) { return now - t <= std::chrono::seconds(1); });
  s.completionFps = s.windowSeconds > 0 ? n / s.windowSeconds : 0;
  return s;
}
} // namespace demo
