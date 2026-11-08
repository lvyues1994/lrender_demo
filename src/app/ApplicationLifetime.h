#pragma once
#include <QThread>
#include <cstddef>
#include <functional>
#include <stdexcept>
namespace demo {
// GUI owner of application progress, not World/presenter/device resources.
// Progress is receipt-driven: a cancelled request is NOT a completed request.
class ApplicationLifetime {
public:
  enum class Phase { Cold, Running, Stopping, Joining, Stopped };
  struct Hooks {
    std::function<void()> stopWork, releaseSession, releaseLoader, closeWindow,
        joinRoot, stopped;
  };
  explicit ApplicationLifetime(Hooks);
  static bool acceptsSharedEvidence(bool required, std::size_t shown) {
    return !required || shown > 0;
  }
  void start();
  void requestExit();
  void onShutdownProgress(bool bootstrapPending, bool sessionStopped,
                          std::size_t loads);
  void onRootJoined();
  void assertSafeDestruction() const;
  Phase phase() const { return phase_; }

private:
  void owner() const;
  void advance();
  Hooks hooks_;
  QThread *thread_;
  Phase phase_ = Phase::Cold;
  bool bootstrap_ = true, sessionStopped_ = false;
  std::size_t loads_ = 0;
};
} // namespace demo
