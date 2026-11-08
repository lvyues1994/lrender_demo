#include "presentation/NativeSurfaceFactory.h"
#include <doctest/doctest.h>
#include <QCoreApplication>
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
    CHECK_THROWS(policy.presenterDestroyed());
    policy.drained();
    CHECK_FALSE(policy.canRelease());
    policy.presenterDestroyed();
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
    policy.constructionFailed();
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
    lease->policy->drained();
    CHECK_THROWS(factory.releaseHost());
    lease->policy->presenterDestroyed();
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
    policy.drained(); policy.presenterDestroyed(); policy.releaseHost();
}
