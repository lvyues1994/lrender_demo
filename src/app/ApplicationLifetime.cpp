#include "ApplicationLifetime.h"
#include <QCoreApplication>
namespace demo {
ApplicationLifetime::ApplicationLifetime(Hooks h)
    : hooks_(std::move(h)), thread_(QThread::currentThread()) {
  if (!QCoreApplication::instance() ||
      thread_ != QCoreApplication::instance()->thread())
    throw std::logic_error("ApplicationLifetime requires GUI owner");
}
void ApplicationLifetime::owner() const {
  if (QThread::currentThread() != thread_)
    throw std::logic_error("application progress outside GUI owner");
}
void ApplicationLifetime::start() {
  owner();
  if (phase_ != Phase::Cold)
    throw std::logic_error("application already started");
  phase_ = Phase::Running;
}
void ApplicationLifetime::requestExit() {
  owner();
  if (phase_ == Phase::Cold)
    throw std::logic_error("application not started");
  if (phase_ != Phase::Running)
    return;
  phase_ = Phase::Stopping;
  if (hooks_.stopWork)
    hooks_.stopWork();
  advance();
}
void ApplicationLifetime::onShutdownProgress(bool bootstrap, bool stopped,
                                             std::size_t loads) {
  owner();
  if (phase_ == Phase::Cold)
    throw std::logic_error("progress before start");
  if (phase_ == Phase::Stopped || phase_ == Phase::Joining)
    return;
  bootstrap_ = bootstrap;
  sessionStopped_ = stopped;
  loads_ = loads;
  advance();
}
void ApplicationLifetime::advance() {
  if (phase_ != Phase::Stopping || bootstrap_ || !sessionStopped_ || loads_)
    return;
  // Publish the barrier BEFORE user hooks: inline join/completion cannot retire
  // twice.
  phase_ = Phase::Joining;
  if (hooks_.releaseSession)
    hooks_.releaseSession();
  if (hooks_.releaseLoader)
    hooks_.releaseLoader();
  if (hooks_.closeWindow)
    hooks_.closeWindow();
  if (hooks_.joinRoot)
    hooks_.joinRoot();
}
void ApplicationLifetime::onRootJoined() {
  owner();
  if (phase_ != Phase::Joining)
    throw std::logic_error("root joined before session/loader retirement");
  phase_ = Phase::Stopped;
  if (hooks_.stopped)
    hooks_.stopped();
}
void ApplicationLifetime::assertSafeDestruction() const {
  owner();
  if (phase_ != Phase::Stopped)
    throw std::logic_error("application/eventloop destroyed before root join");
}
} // namespace demo
