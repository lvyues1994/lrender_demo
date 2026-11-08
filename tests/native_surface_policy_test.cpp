#include "presentation/NativeSurfaceFactory.h"
#include "render/SessionState.h"
#include <doctest/doctest.h>
#include <QCoreApplication>
#include <QPlatformSurfaceEvent>
#include <QEventLoop>
#include <QThread>
#include <QPointer>
#include <QWindow>
#include <QGuiApplication>
#include <QTimer>
#include <lqtexec/EventLoopContext.h>
#include <lexec/execution.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
namespace {
auto const instance = reinterpret_cast<VkInstance>(std::uintptr_t{1});
auto const surface = reinterpret_cast<VkSurfaceKHR>(std::uintptr_t{2});
struct EmptyPresenter {void flush(){}};
void finish(demo::NativeSurfacePolicy& policy) {
 auto p=std::shared_ptr<demo::NativeSurfacePolicy>(&policy,[](auto*){});
 auto presenter=std::make_shared<EmptyPresenter>();
 auto worker=std::async(std::launch::async,[&]{demo::NativePresenterCleanup::run(p,presenter,[]{},[&]{CHECK_FALSE(policy.canRelease());});});
 worker.get();
}
}
TEST_CASE("native_surface/unique-transfer-drain-destroy-host") {
    demo::NativeSurfacePolicy policy;
    CHECK(policy.canRelease());
    VkInstance seen = VK_NULL_HANDLE;
    CHECK(policy.transfer(instance, [&](VkInstance value) { seen = value; return surface; }) == surface);
    CHECK(seen == instance);
    CHECK(policy.hostAlive());
    CHECK_FALSE(policy.canRelease());
    CHECK_THROWS(policy.transfer(instance, [](VkInstance) { return surface; }));
    CHECK_THROWS(policy.releaseHost());
    CHECK_FALSE(policy.canRelease());
    finish(policy);
    CHECK(policy.hostAlive());
    policy.releaseHost();
    CHECK_FALSE(policy.hostAlive());
    CHECK_THROWS(policy.transfer(instance, [](VkInstance) { return surface; }));
}
TEST_CASE("native_surface/null-surface-is-error-and-not-transferred") {
    demo::NativeSurfacePolicy policy;
    CHECK_THROWS_WITH(policy.transfer(instance, [](VkInstance) { return VK_NULL_HANDLE; }), "Surface: native creator returned VK_NULL_HANDLE");
    CHECK(policy.canRelease());
    policy.releaseHost();
}
TEST_CASE("native_surface/invalid-lease-never-calls-driver") {
    demo::NativeSurfacePolicy policy;
    policy.invalidate();
    bool called = false;
    CHECK_THROWS(policy.transfer(instance, [&](VkInstance) { called = true; return surface; }));
    CHECK_FALSE(called);
    policy.releaseHost();
}
TEST_CASE("native_surface/gui-release-thread-enforced") {
    demo::NativeSurfacePolicy policy;
    auto rejected = std::async(std::launch::async, [&] {
        try { policy.releaseHost(); return false; } catch (std::logic_error const &) { return true; }
    });
    CHECK(rejected.get());
    CHECK(policy.hostAlive());
    policy.releaseHost();
}
TEST_CASE("native_surface/constructor-unwind-releases-adopted-surface") {
    demo::NativeSurfacePolicy policy;
    policy.transfer(instance, [](VkInstance) { return surface; });
    auto p=std::shared_ptr<demo::NativeSurfacePolicy>(&policy,[](auto*){});
    auto construction=std::async(std::launch::async,[&]{demo::NativePresenterCleanup::construct(p,[]()->std::shared_ptr<EmptyPresenter>{throw std::runtime_error("constructor unwound");});});
    CHECK_THROWS(construction.get());
    CHECK(policy.canRelease());
    policy.releaseHost();
}

TEST_CASE("native_surface/factory-host-survives-presenter-and-stale-lease-rejected") {
    demo::NativeSurfaceFactory factory([](VkInstance value) { return value == instance ? surface : VK_NULL_HANDLE; });
    auto lease = factory.lease();
    QPointer<QWindow> actualHost;
    for (auto *window : QGuiApplication::allWindows())
        if (window->title().startsWith("lrender native probe ")) actualHost = window;
    REQUIRE_FALSE(actualHost.isNull());
    auto created = std::async(std::launch::async, [&] { return lease->createSurface(instance); });
    CHECK(created.get() == surface);
    CHECK_THROWS(factory.releaseHost());
    CHECK(lease->policy->hostAlive());
    CHECK_FALSE(actualHost.isNull());
    finish(*lease->policy);
    factory.releaseHost();
    CHECK(actualHost.isNull());
    CHECK_FALSE(lease->policy->hostAlive());
    CHECK_THROWS(lease->createSurface(instance));
}
TEST_CASE("native_surface/event-loop-schedule-runs-on-gui-without-blocking") {
    auto context = std::make_unique<lqtexec::EventLoopContext>();
    auto scheduler = context->get_scheduler();
    QEventLoop loop;
    auto expected = QThread::currentThread();
    std::atomic<QThread *> ran{nullptr};
    auto completion = std::async(std::launch::async, [&] {
        return lexec::sync_wait(lexec::schedule(scheduler) | lexec::then([&] {
            ran.store(QThread::currentThread());
            QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
        })).has_value();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    // Keep the borrowed scheduler's context alive until the worker has definitely
    // finished. On timeout do not unwind into a blocking future destructor or
    // delete a context before an unscheduled worker has connected its operation.
    if (completion.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cerr << "EventLoopContext schedule exceeded bounded test wait\n";
        std::_Exit(2);
    }
    CHECK(completion.get());
    context.reset();
    CHECK(ran.load() == expected);
}

TEST_CASE("native_surface/create-in-progress-prevents-host-release-without-gui-wait") {
    demo::NativeSurfacePolicy policy;
    std::promise<void> entered, resume;
    auto gate = resume.get_future();
    auto ready = entered.get_future();
    auto operation = std::async(std::launch::async, [&] {
        return policy.transfer(instance, [&](VkInstance) { entered.set_value(); gate.wait(); return surface; });
    });
    auto status = ready.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) { resume.set_value(); operation.wait(); FAIL("surface driver did not start"); return; }
    CHECK_FALSE(policy.canRelease());
    CHECK_THROWS(policy.releaseHost());
    resume.set_value();
    CHECK(operation.get() == surface);
    finish(policy); policy.releaseHost();
}

TEST_CASE("native_surface/p4-invalidation-notifies-once-reentrant-and-serial") {
 auto policy=std::make_shared<demo::NativeSurfacePolicy>();auto old=policy->invalidationSerial();int notifications=0;
 policy->setInvalidationHandler([&](std::uint64_t serial){++notifications;CHECK(serial==old+1);CHECK_FALSE(policy->hostAlive());});
 policy->invalidate();CHECK(notifications==1);CHECK(policy->invalidationSerial()==old+1);
 policy->invalidate();CHECK(notifications==1);
 CHECK_THROWS(policy->transfer(instance,[](VkInstance){return surface;}));
 int replay=0;policy->setInvalidationHandler([&](auto){++replay;});CHECK(replay==1);
 policy->releaseHost();
}
TEST_CASE("native_surface/p4-worker-cleanup-enforces-settle-flush-release-destroy") {
 auto policy=std::make_shared<demo::NativeSurfacePolicy>();policy->transfer(instance,[](VkInstance){return surface;});
 struct Presenter { std::vector<std::string>* stages; void flush(){stages->push_back("flush");} ~Presenter(){stages->push_back("destroy");} };
 std::vector<std::string> stages;
 auto presenter=std::shared_ptr<Presenter>(new Presenter{&stages});
 auto worker=std::async(std::launch::async,[&]{demo::NativePresenterCleanup::run(policy,presenter,[&]{stages.push_back("settle");},[&]{CHECK_FALSE(policy->canRelease());stages.push_back("release-target-and-other-owners");});});
 worker.get();
 CHECK(stages==std::vector<std::string>{"settle","flush","release-target-and-other-owners","destroy"});
 CHECK(policy->canRelease());policy->releaseHost();
}

TEST_CASE("native_surface/p4-cleanup-refuses-gui-and-surviving-presenter-owner") {
 auto policy=std::make_shared<demo::NativeSurfacePolicy>();policy->transfer(instance,[](VkInstance){return surface;});
 auto presenter=std::make_shared<EmptyPresenter>();
 CHECK_THROWS(demo::NativePresenterCleanup::run(policy,presenter,[]{},[]{}));CHECK_FALSE(policy->canRelease());
 auto other=presenter;
 auto worker=std::async(std::launch::async,[&]{demo::NativePresenterCleanup::run(policy,presenter,[]{},[]{});});
 CHECK_THROWS(worker.get());CHECK_FALSE(policy->canRelease());other.reset();
 auto finish=std::async(std::launch::async,[&]{demo::NativePresenterCleanup::run(policy,presenter,[]{},[]{});});finish.get();
 CHECK(policy->canRelease());policy->releaseHost();
}

TEST_CASE("native_surface/p4-construction-unwind-is-encapsulated-and-lease-retired") {
 auto policy=std::make_shared<demo::NativeSurfacePolicy>();
 auto worker=std::async(std::launch::async,[&]{
  return demo::NativePresenterCleanup::construct(policy,[&]() -> std::shared_ptr<EmptyPresenter> {
   policy->transfer(instance,[](VkInstance){return surface;});throw std::runtime_error("driver constructor unwound");
  });
 });
 CHECK_THROWS(worker.get());CHECK(policy->canRelease());
 bool driverCalled=false;CHECK_THROWS(policy->transfer(instance,[&](VkInstance){driverCalled=true;return surface;}));CHECK_FALSE(driverCalled);
 if(policy->canRelease()) policy->releaseHost();
}

TEST_CASE("native_surface/p4-actual-factory-platform-event-notifies-session-boundary") {
 bool called=false;demo::NativeSurfaceFactory factory([&](VkInstance){called=true;return surface;});
 auto lease=factory.lease();int notifications=0;
 demo::SessionState session;session.setViewport({{2,2},{2,2},1});session.start();
 REQUIRE(session.presentationReady(session.snapshot()->generation,demo::PresentationPath::VulkanShared));
 auto generation=session.snapshot()->generation;
 lease->policy->setInvalidationHandler([&](auto){
  ++notifications;CHECK(QThread::currentThread()==QCoreApplication::instance()->thread());
  session.presentationInvalidated({demo::ErrorStage::Surface,true,"native host invalidated","platform event"});
 });
 QWindow* host=nullptr;for(auto window:QGuiApplication::allWindows()) if(window->title().startsWith("lrender native probe ")) host=window;
 REQUIRE(host);QPlatformSurfaceEvent event(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);QCoreApplication::sendEvent(host,&event);
 CHECK(notifications==1);CHECK_FALSE(lease->policy->hostAlive());CHECK_THROWS(lease->createSurface(instance));CHECK_FALSE(called);
 CHECK_FALSE(session.canSubmit());CHECK(session.snapshot()->generation==generation+1);
 CHECK_FALSE(session.presentationReady(generation,demo::PresentationPath::VulkanShared));
 factory.releaseHost();CHECK(notifications==1);
}

TEST_CASE("native_surface/p6-throwing-invalidation-handler-is-contained-platform-error") {
 demo::NativeSurfacePolicy policy;static QString captured;captured.clear();
 auto old=qInstallMessageHandler(+[](QtMsgType,QMessageLogContext const&,QString const& text){captured+=text;});
 policy.setInvalidationHandler([](auto){throw std::runtime_error("observer failed");});
 CHECK_NOTHROW(policy.invalidate());
 qInstallMessageHandler(old);
 CHECK_FALSE(policy.hostAlive());CHECK(captured.contains("Platform"));CHECK(captured.contains("observer failed"));policy.releaseHost();
}

TEST_CASE("native_surface/p9-timeout-never-calls-late-driver-with-unwound-instance") {
 int driverCalls=0;
 demo::NativeSurfaceFactory factory([&](VkInstance)->VkSurfaceKHR{++driverCalls;throw std::runtime_error("must not touch an unwound instance");},nullptr,true);
 auto lease=factory.lease();
 auto result=std::async(std::launch::async,[&]{return demo::NativePresenterCleanup::construct(lease->policy,[&]{return lease->createSurface(instance);});});
 // Deliberately do not dispatch GUI events while its worker queue wait expires.
 REQUIRE(result.wait_for(std::chrono::seconds(4))==std::future_status::ready);
 CHECK_THROWS_WITH(result.get(),"Platform: surface GUI handshake timeout");
 QCoreApplication::processEvents();
 CHECK(driverCalls==0);
 CHECK(lease->policy->canRelease());factory.releaseHost();
}

TEST_CASE("native-policy/unavailable-physical-extent-is-zero-not-QSize-invalid-minus-one") {
  demo::NativeSurfaceFactory factory([](VkInstance){return reinterpret_cast<VkSurfaceKHR>(std::uintptr_t{2});},nullptr,true);
  factory.setViewport(QSize(0,0));
  auto extent=factory.physicalExtent();
  CHECK(extent.width()==0);CHECK(extent.height()==0);
}
