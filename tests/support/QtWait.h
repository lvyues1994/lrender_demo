#pragma once
#include <QEventLoop>
#include <QTimer>
#include <functional>
namespace demo::test {
inline bool until(std::function<bool()> predicate, int timeout = 8000) {
  if (predicate())
    return true;
  QEventLoop loop;
  QTimer poll, deadline;
  bool done = false;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (predicate()) {
      done = true;
      loop.quit();
    }
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  deadline.setSingleShot(true);
  poll.start(1);
  deadline.start(timeout);
  loop.exec();
  return done;
}
} // namespace demo::test
