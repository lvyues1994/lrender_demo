#include "app/ApplicationLifetime.h"
#include "support/QtWait.h"
#include <QTimer>
#include <doctest/doctest.h>
using namespace demo;
TEST_CASE(
    "shutdown/lifecycle-waits-for-late-loader-receipt-after-session-stop") {
  unsigned stops = 0, releases = 0, loaders = 0, windows = 0, joins = 0,
           exits = 0;
  ApplicationLifetime life({[&] { ++stops; }, [&] { ++releases; },
                            [&] { ++loaders; }, [&] { ++windows; },
                            [&] { ++joins; }, [&] { ++exits; }});
  life.start();
  life.onShutdownProgress(false, false, 1);
  life.requestExit();
  life.requestExit();
  life.onShutdownProgress(false, true, 1);
  CHECK(stops == 1);
  CHECK(releases == 0);
  CHECK(joins == 0);
  unsigned ticks = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
  heartbeat.start(1);
  REQUIRE(test::until([&] { return ticks >= 5; }));
  life.onShutdownProgress(false, true, 0);
  CHECK(releases == 1);
  CHECK(loaders == 1);
  CHECK(windows == 1);
  CHECK(joins == 1);
  CHECK(exits == 0);
  life.onShutdownProgress(false, true, 0);
  life.requestExit();
  CHECK(joins == 1);
  life.onRootJoined();
  CHECK(exits == 1);
  CHECK_NOTHROW(life.assertSafeDestruction());
}
TEST_CASE(
    "shutdown/"
    "early-close-never-retires-live-bootstrap-and-invalid-join-rejected") {
  unsigned stop = 0, retired = 0;
  ApplicationLifetime life(
      {[&] { ++stop; }, [&] { ++retired; }, {}, {}, {}, {}});
  life.start();
  CHECK_THROWS_AS(life.assertSafeDestruction(), std::logic_error);
  CHECK_THROWS_AS(life.onRootJoined(), std::logic_error);
  life.requestExit();
  life.onShutdownProgress(true, true, 0);
  CHECK(retired == 0);
  life.onShutdownProgress(false, true, 0);
  CHECK(retired == 1);
  CHECK(stop == 1);
  life.onRootJoined();
  CHECK_NOTHROW(life.assertSafeDestruction());
}
TEST_CASE("shutdown/inline-join-reentrancy-has-exactly-one-retirement") {
  unsigned release = 0, exit = 0;
  ApplicationLifetime *p = nullptr;
  ApplicationLifetime life({{},
                            [&] { ++release; },
                            {},
                            {},
                            [&] { p->onRootJoined(); },
                            [&] { ++exit; }});
  p = &life;
  life.start();
  life.onShutdownProgress(false, true, 0);
  life.requestExit();
  life.requestExit();
  CHECK(release == 1);
  CHECK(exit == 1);
  CHECK_NOTHROW(life.assertSafeDestruction());
}

TEST_CASE("shutdown/shared-zero-evidence-is-never-success") {
  CHECK_FALSE(ApplicationLifetime::acceptsSharedEvidence(true, 0));
  CHECK(ApplicationLifetime::acceptsSharedEvidence(true, 1));
  CHECK(ApplicationLifetime::acceptsSharedEvidence(false, 0));
}
