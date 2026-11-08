#pragma once
#include "TinyrendererAdapter.h"
#include <lqtexec/ObjectScope.h>
#include <lqtexec/ThreadPoolScheduler.h>
namespace demo {
enum class LoadStatus { Loaded, Cancelled, Failed };
struct LoadOutcome {
  AssetRequestId request = 0;
  LoadStatus status = LoadStatus::Cancelled;
  CpuAssetPtr asset;
  std::optional<RenderError> error;
};
struct LoadTicket {
  AssetRequestId request = 0;
  std::function<void()> cancel;
};
// GUI-owned service; application pool/scheduler/root stay live through all
// receipts. Injected Decode may run concurrently on at most two workers, with
// cooperative stop. Session injection is nonowning: service must outlive
// Session::onStopped. Tickets may outlive completion; cancel is repeatable, not
// producer destruction.
// Destruction requires GUI owner and assertIdle(); active destruction is fatal.
// Pool/root/GUI context remain alive through the application root join.
class AssetLoader {
public:
  using AppToken =
      decltype(std::declval<lexec::counting_scope &>().get_token());
  using Decode = std::function<Result<CpuAssetPtr>(AssetEntry const &,
                                                   std::function<bool()>)>;
  AssetLoader(QThreadPool *, lqtexec::EventLoopScheduler, AppToken,
              Decode = {});
  ~AssetLoader();
  AssetLoader(AssetLoader const &) = delete;
  AssetLoader &operator=(AssetLoader const &) = delete;
  LoadTicket beginLoad(AssetEntry, AssetRequestId,
                       std::function<void(LoadOutcome)>);
  std::size_t inFlight() const;
  void assertIdle() const;
  // Queued GUI notification after ALL producer receipts retire; cancellation is
  // not idle.
  void setIdleHandler(std::function<void()>);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace demo
