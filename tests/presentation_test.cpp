#include "presentation/ReadbackAdapter.h"
#include "support/FakePresentation.h"
#include <doctest/doctest.h>
#include <QCoreApplication>
#include <QImage>
#include <QEventLoop>
#include <QTimer>
using namespace demo;
namespace {
void tick() { QEventLoop loop; QTimer::singleShot(5,&loop,&QEventLoop::quit); loop.exec(); }
void prepare(ReadbackAdapter &a) {
 bool prepared=false; a.preparePath(PresentationPath::Readback,{},[&](auto r){prepared=std::holds_alternative<std::monostate>(r);});
 tick(); REQUIRE(prepared); bool probed=false;
 tc::DeviceInfo info{}; info.backend="vulkan";
 a.probe(info,[&](auto r){probed=std::holds_alternative<ProbeReport>(r);}); tick(); REQUIRE(probed);
 PresentationAdapter& boundary=a; boundary.setFrameContext(4,{{2,1},{2,1},1});
}
OwnedFrame frame(SessionGeneration generation=4, int width=2) {
 auto image=std::make_shared<lr::render::ImageRgba8>(); image->extent={width,1};
 image->pixels={0xff0000ffu,0xff00ff00u}; return {{lr::core::ViewId{1},generation,1},image};
}
}
TEST_CASE("presentation/prepare-and-empty-drain-are-not-inline") {
 ReadbackAdapter a({}); CHECK_THROWS(a.describeTarget({{2,1},{2,1},1}));
 bool done=false; a.preparePath(PresentationPath::Readback,{},[&](auto r){CHECK(std::holds_alternative<std::monostate>(r));done=true;});
 CHECK_FALSE(done); tick(); CHECK(done);
 CHECK(std::holds_alternative<lr::render::ImageTargetDesc>(a.describeTarget({{2,1},{2,1},1})));
 done=false; a.beginDrain([&](auto r){CHECK(std::holds_alternative<std::monostate>(r));done=true;});
 CHECK_FALSE(done); tick(); CHECK(done);
 int releases=0; a.releasePath([&](auto r){CHECK(std::holds_alternative<std::monostate>(r));++releases;}); tick();
 a.releasePath([&](auto r){CHECK(std::holds_alternative<std::monostate>(r));++releases;}); tick(); CHECK(releases==2);
}
TEST_CASE("presentation/owns-pixels-and-qimage-copy-after-owner-release") {
 std::optional<OwnedFrame> delivered;
 ReadbackAdapter a({[&](auto f){delivered=std::move(f);},{}}); prepare(a);
 auto source=frame(); std::weak_ptr<lr::render::ImageRgba8 const> weak=source.image;
 a.consumeImage(source); source.image.reset(); REQUIRE(delivered.has_value()); CHECK_FALSE(weak.expired());
 auto copy=ReadbackAdapter::copyImage(*delivered->image); delivered.reset(); CHECK(weak.expired());
 CHECK(copy.pixelColor(0,0)==QColor(255,0,0,255)); CHECK(copy.pixelColor(1,0)==QColor(0,255,0,255));
}
TEST_CASE("presentation/frame-epoch-extent-and-drain-admission") {
 int images=0,errors=0; ReadbackAdapter a({[&](auto){++images;},[&](auto){++errors;}}); prepare(a);
 a.consumeImage(frame(3)); a.consumeImage(frame(4,1)); CHECK(images==0);
 a.consumeImage(frame()); CHECK(images==1);
 auto malformed=frame(); auto broken=std::make_shared<lr::render::ImageRgba8>(*malformed.image); broken->pixels.clear();malformed.image=broken;
 a.consumeImage(malformed); CHECK(errors==1); CHECK(images==1);
 a.beginDrain([](auto){}); a.consumeImage(frame()); CHECK(images==1); tick();
 a.releasePath([](auto){}); tick(); CHECK_THROWS(a.describeTarget({}));
}
TEST_CASE("presentation/held-boundary-uses-production-lifecycle-not-fake-state") {
 FakePresentation a; int prepared=0,drained=0;
 a.preparePath(PresentationPath::Readback,{},[&](auto){++prepared;}); tick(); CHECK(prepared==0);
 CHECK_THROWS(a.describeTarget({}));
 bool heartbeat=false; QTimer::singleShot(0,[&]{heartbeat=true;}); tick(); CHECK(heartbeat);
 a.complete(); tick(); CHECK(prepared==1);
 a.beginDrain([&](auto){++drained;}); tick(); CHECK(drained==0);
 a.complete(); tick(); CHECK(drained==1);
 int released=0;a.releasePath([&](auto){++released;});tick();a.complete();tick(); CHECK(released==1);
}
TEST_CASE("presentation/close-during-prepare-cancels-cooperatively-once") {
 FakePresentation a; int prepares=0,drains=0;
 a.preparePath(PresentationPath::Readback,{},[&](auto r){CHECK(std::holds_alternative<RenderError>(r));++prepares;});
 a.beginDrain([&](auto){++drains;});tick();CHECK(prepares==1);
 CHECK(a.stages()==std::vector<std::string>{"prepare"}); // no drain until candidate completion
 a.complete();tick();CHECK(prepares==1); CHECK(drains==0);
 a.complete();tick();CHECK(drains==1);
}
TEST_CASE("presentation/unsupported-is-observable-not-fake-shared-success") {
 int errors=0;ReadbackAdapter a({{},[&](auto e){CHECK(e.stage==ErrorStage::Surface);++errors;}});
 bool failed=false; a.preparePath(PresentationPath::VulkanShared,{},[&](auto r){failed=std::holds_alternative<RenderError>(r);});
 CHECK_FALSE(failed);tick();CHECK(failed);CHECK(errors==1);CHECK_THROWS(a.describeTarget({}));
}
TEST_CASE("presentation/fallback-order-target-rebuild-and-close-priority") {
 PresentationTransition transition;
 RenderError cause{ErrorStage::Probe,true,"Unsupported","importsMemory unavailable"};
 transition.fallback(cause); CHECK(transition.action()==TransitionAction::AwaitOldFrames);
 REQUIRE(transition.reason().has_value()); CHECK(transition.reason()->cause==cause.cause);
 transition.completed(TransitionAction::AwaitOldFrames);CHECK(transition.action()==TransitionAction::DrainOld);
 CHECK_THROWS(transition.completed(TransitionAction::ReleaseOld));
 transition.completed(TransitionAction::DrainOld);CHECK(transition.action()==TransitionAction::DestroyOldTarget);
 transition.completed(TransitionAction::DestroyOldTarget);CHECK(transition.action()==TransitionAction::ReleaseOld);
 transition.completed(TransitionAction::ReleaseOld);CHECK(transition.action()==TransitionAction::PrepareNew);
 transition.close();transition.completed(TransitionAction::PrepareNew);
 CHECK(transition.action()==TransitionAction::DrainOld); // acquired candidate must retire, never publish
 transition.completed(TransitionAction::DrainOld);transition.completed(TransitionAction::DestroyOldTarget);
 transition.completed(TransitionAction::ReleaseOld);CHECK(transition.action()==TransitionAction::Closed);
}

TEST_CASE("presentation/boundary-error-and-early-release-use-production-policy") {
 FakePresentation a;int errors=0;
 a.preparePath(PresentationPath::Readback,{},[&](auto r){CHECK(std::holds_alternative<RenderError>(r));++errors;});
 a.complete(RenderError{ErrorStage::Probe,true,"Unsupported","fake platform failure"});tick();CHECK(errors==1);
 CHECK_THROWS(a.describeTarget({{2,1},{2,1},1}));
 int releaseErrors=0;a.releasePath([&](auto r){CHECK(std::holds_alternative<RenderError>(r));++releaseErrors;});tick();CHECK(releaseErrors==1);
 a.beginDrain([](auto){});a.complete();tick();a.releasePath([](auto){});a.complete();tick();
 CHECK(a.stages()==std::vector<std::string>{"prepare","drain","release"});
}
TEST_CASE("presentation/drain-completion-survives-wrapper-destruction") {
 int done=0;auto a=std::make_unique<ReadbackAdapter>(PresentationEvents{});
 a->beginDrain([&](auto r){CHECK(std::holds_alternative<std::monostate>(r));++done;});a.reset();CHECK(done==0);tick();CHECK(done==1);
}
TEST_CASE("presentation/successful-fallback-publishes-only-after-new-prepare") {
 PresentationTransition t;t.fallback({ErrorStage::Probe,true,"Unsupported","same Vulkan renderer readback"});
 for(auto action:{TransitionAction::AwaitOldFrames,TransitionAction::DrainOld,TransitionAction::DestroyOldTarget,TransitionAction::ReleaseOld,TransitionAction::PrepareNew}) {
  REQUIRE(t.action()==action);t.completed(action);
 }
 CHECK(t.action()==TransitionAction::PublishNew);t.completed(TransitionAction::PublishNew);CHECK(t.action()==TransitionAction::Idle);
 t.close();t.fallback({ErrorStage::Surface,true,"late","ignored"});CHECK(t.action()==TransitionAction::AwaitOldFrames);
}

TEST_CASE("presentation/boundary-duplicate-completion-is-fenced-and-queued") {
 int callbacks=0;
 ReadbackAdapter a({},[](std::string,Done done){done(std::monostate{});done(std::monostate{});});
 a.preparePath(PresentationPath::Readback,{},[&](auto){++callbacks;});CHECK(callbacks==0);tick();CHECK(callbacks==1);
}
TEST_CASE("presentation/qimage-row-order-and-all-four-channels") {
 lr::render::ImageRgba8 image;image.extent={1,2};image.pixels={0x11223344u,0x55667788u};
 auto copy=ReadbackAdapter::copyImage(image);image.pixels.clear();
 CHECK(copy.pixelColor(0,0)==QColor(0x44,0x33,0x22,0x11));CHECK(copy.pixelColor(0,1)==QColor(0x88,0x77,0x66,0x55));
}

TEST_CASE("presentation/p6-drain-failure-cannot-prepare-before-retry") {
 FakePresentation a;a.preparePath(PresentationPath::Readback,{},[](auto){});a.complete();tick();
 a.beginDrain([](auto){});a.complete(RenderError{ErrorStage::Shutdown,true,"drain failed","held work"});tick();
 bool rejected=false;a.preparePath(PresentationPath::Readback,{},[&](auto r){rejected=std::holds_alternative<RenderError>(r);});tick();
 CHECK(rejected);CHECK(a.stages()==std::vector<std::string>{"prepare","drain"});
 a.beginDrain([](auto){});a.complete();tick();a.releasePath([](auto){});a.complete();tick();
}
TEST_CASE("presentation/p6-frame-context-cannot-go-back-or-reuse-retired-epoch") {
 ReadbackAdapter a({});prepare(a);
 CHECK_THROWS_AS(a.setFrameContext(3,{{2,1},{2,1},1}),std::invalid_argument);
 CHECK_NOTHROW(a.setFrameContext(4,{{2,1},{2,1},1}));
 CHECK_THROWS_AS(a.setFrameContext(4,{{3,1},{3,1},1}),std::invalid_argument);
 a.beginDrain([](auto){});tick();a.releasePath([](auto){});tick();
 bool done=false;a.preparePath(PresentationPath::Readback,{},[&](auto){done=true;});tick();REQUIRE(done);
 tc::DeviceInfo info{};a.probe(info,[](auto){});tick();
 CHECK_THROWS_AS(a.setFrameContext(4,{{2,1},{2,1},1}),std::invalid_argument);
 CHECK_NOTHROW(a.setFrameContext(5,{{2,1},{2,1},1}));
}
