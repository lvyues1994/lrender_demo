#include "presentation/NativeSurfaceFactory.h"
#include <lr/present/VulkanPresenter.h>
#include <lr/runtime/Runtime.h>
#include <lr/render/Renderer.h>
#include <lr/core/Primitives.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, char const *reason) { if (!ok) throw std::runtime_error(reason); }
struct Snapshot { QSize pixels; double dpr; bool exposed; };
template<class Bytes> std::string hex(Bytes const &bytes) {
    std::ostringstream text;
    for (auto byte : bytes) text << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return text.str();
}
// Only the worker waits. The GUI callback is short and contains no GPU waits.
Snapshot guiCall(QApplication &app, demo::NativeSurfaceFactory &factory, int action = 0) {
    auto promise = std::make_shared<std::promise<Snapshot>>();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto result = promise->get_future();
    QMetaObject::invokeMethod(&app, [&factory, promise, cancelled, action] {
        if (cancelled->load()) return;
        try {
            if (action == 1) factory.setViewport({208, 144});
            if (action == 2) factory.setVisible(false);
            if (action == 3) factory.setVisible(true);
            if (action == 4) factory.setViewport({0, 0});
            if (action == 5) factory.setViewport({160, 120});
            promise->set_value({factory.physicalExtent(), factory.devicePixelRatio(), factory.exposed()});
        } catch (...) { promise->set_exception(std::current_exception()); }
    }, Qt::QueuedConnection);
    if (result.wait_for(3s) != std::future_status::ready) {
        cancelled->store(true);
        throw std::runtime_error("Platform: GUI handshake timed out");
    }
    return result.get();
}
void checkVulkanAvailable() {
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        throw std::runtime_error("Capability: Vulkan instance unavailable");
    uint32_t count = 0;
    auto result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (result != VK_SUCCESS || count == 0) throw std::runtime_error("Capability: no Vulkan physical device");
}
} // namespace
int main(int argc, char **argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    bool requireShared = false;
    int frames = 24;
    for (int i = 1; i < argc; ++i) {
        auto arg = std::string(argv[i]);
        if (arg == "--require-shared") requireShared = true;
        else if (arg == "--frames" && i + 1 < argc) {
            try { frames = std::stoi(argv[++i]); } catch (...) { std::cerr << "Invalid --frames\n"; return 1; }
        } else { std::cerr << "Unknown argument: " << arg << '\n'; return 1; }
    }
    if (frames < 12 || frames > 1000) { std::cerr << "--frames must be 12..1000\n"; return 1; }
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        std::cerr << "Platform: Unsupported, no DISPLAY\n"; return requireShared ? 1 : 77;
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    std::unique_ptr<demo::NativeSurfaceFactory> factory;
    try { factory = std::make_unique<demo::NativeSurfaceFactory>(); }
    catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return (requireShared || QGuiApplication::platformName() == "xcb") ? 1 : 77;
    }
    auto lease = factory->lease();
    std::atomic<std::uint64_t> heartbeat{0};
    QElapsedTimer clock; clock.start();
    qint64 previous = 0, maxGap = 0;
    QTimer ticker;
    ticker.setTimerType(Qt::PreciseTimer);
    QObject::connect(&ticker, &QTimer::timeout, &app, [&] {
        auto now = clock.elapsed(); maxGap = std::max(maxGap, now - previous); previous = now; ++heartbeat;
    });
    ticker.start(2);
    // A stuck Vulkan/driver call cannot leave this automated probe alive indefinitely.
    std::mutex watchdogMutex;
    std::condition_variable watchdogWake;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(watchdogMutex);
        if (!watchdogWake.wait_for(lock, 45s, [&] { return finished; })) {
            std::cerr << "Synchronization: probe exceeded 45s process deadline\n";
            std::_Exit(2);
        }
    });
    int code = 1;
    std::thread worker([&] {
        std::unique_ptr<lr::runtime::Runtime> runtime;
        std::unique_ptr<lr::render::Renderer> renderer;
        std::shared_ptr<lr::present::VulkanPresenter> presenter;
        std::optional<lr::core::RenderTargetId> target;
        std::deque<lr::render::FrameSender> pending;
        bool constructionAttempted = false, deviceAvailable = false;
        std::uint64_t submitted = 0, completed = 0, maxInFlight = 0;
        auto measured = [&](char const *phase, auto &&function) {
            auto before = heartbeat.load(); auto start = Clock::now();
            function();
            auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            std::cout << "phase=" << phase << " wallMs=" << ms << " guiTicks=" << heartbeat.load() - before << '\n';
        };
        auto drain = [&] {
            std::exception_ptr error;
            while (!pending.empty()) {
                auto sender = std::move(pending.front()); pending.pop_front();
                try {
                    // A separate waiter lets this control worker enforce a per-frame deadline.
                    // On a stuck driver fail the automated process, rather than unwinding
                    // a still-live native host or leaving an unjoinable waiter behind.
                    auto completion = std::async(std::launch::async, [sender = std::move(sender)]() mutable {
                        return lexec::sync_wait(std::move(sender));
                    });
                    if (completion.wait_for(5s) != std::future_status::ready) {
                        std::cerr << "Synchronization: frame completion exceeded 5s deadline\n";
                        std::_Exit(2);
                    }
                    auto result = completion.get();
                    require(result.has_value(), "Synchronization: submitted frame stopped");
                    auto const &frame = std::get<0>(*result);
                    require(frame.views.size() == 1 && !frame.views.front().image,
                            "Synchronization: window frame result contract failed");
                } catch (...) { if (!error) error = std::current_exception(); }
                ++completed;
            }
            if (error) std::rethrow_exception(error);
        };
        auto awaitExposure = [&](bool expected) {
            auto deadline = Clock::now() + 2s;
            do {
                auto snapshot = guiCall(app, *factory);
                if (snapshot.exposed == expected) return snapshot;
                std::this_thread::sleep_for(10ms); // bounded observation, never a GUI/GPU busy wait
            } while (Clock::now() < deadline);
            throw std::runtime_error("Platform: native server viewability transition exceeded 2s");
        };
        auto cleanup = [&] {
            // All FrameSenders have resolved (including failures). No recorder can still
            // own a Writing slot; upstream failure path calls discardShared().
            drain();
            if (presenter) {
                measured("flush", [&] { presenter->flush(); });
                lease->policy->drained();
                auto stats = presenter->stats();
                std::cout << "shown=" << stats.shown << " sharedShown=" << stats.sharedShown
                          << " dropped=" << stats.dropped << " submitted=" << submitted
                          << " completed=" << completed << " maxInFlight=" << maxInFlight << '\n';
                require(stats.sharedShown > 0 || submitted == 0, "Synchronization: sharedShown stayed zero");
                if (code == 0) require(stats.shown + stats.dropped == submitted,
                                       "Synchronization: presenter accounting lost a submitted frame");
                if (target) { renderer->targets().destroy(*target); target.reset(); }
                measured("destruction", [&] { renderer.reset(); runtime.reset(); presenter.reset(); });
                lease->policy->presenterDestroyed();
            } else {
                if (constructionAttempted) lease->policy->constructionFailed();
                measured("destruction-before-presenter", [&] { renderer.reset(); runtime.reset(); });
            }
        };
        try {
            checkVulkanAvailable(); deviceAvailable = true;
            measured("renderer-init", [&] {
                lr::runtime::RuntimeConfig rc; rc.framePoolThreads = 2;
                runtime = lr::runtime::makeRuntime(rc);
                lr::render::RendererConfig config; config.backend = "vulkan";
                config.runtime = runtime.get(); config.framesInFlight = 2;
                renderer = lr::render::makeRenderer(config);
            });
            auto const &info = renderer->deviceInfo();
            std::cout << "renderer=" << info.deviceName << " backend=" << info.backend
                      << " importsMemory=" << info.importsMemory << " syncFiles=" << info.syncFiles << '\n';
            if (info.gpu) std::cout << "deviceUUID=" << hex(info.gpu->device) << " driverUUID=" << hex(info.gpu->driver) << '\n';
            measured("presenter-init", [&] {
                lr::present::VulkanPresenterConfig pc;
                pc.instanceExtensions = lease->instanceExtensions;
                pc.createSurface = lease->createSurface;
                pc.shareBuffers = true; pc.deviceNameContains = info.deviceName;
                constructionAttempted = true;
                presenter = lr::present::makeVulkanPresenter(std::move(pc));
            });
            // Real implementation compares BOTH device and driver UUID, not names.
            auto shared = presenter->sharesBuffersWith(info);
            std::cout << "presenter=" << presenter->deviceName() << " sharesBuffersWith=" << shared
                      << " bridge=public-xcb Qt=" << qVersion() << '\n';
            require(info.gpu.has_value() && info.importsMemory && info.syncFiles && shared,
                    "Capability: renderer/presenter UUID or external-memory/sync-file sharing unavailable");
            auto snap = awaitExposure(true);
            require(snap.exposed && !snap.pixels.isEmpty(), "Platform: native window was not exposed");
            std::cout << "initialPhysical=" << snap.pixels.width() << 'x' << snap.pixels.height() << " DPR=" << snap.dpr << '\n';
            target = renderer->targets().createWindow({{snap.pixels.width(), snap.pixels.height()}, presenter});
            auto capture = presenter->captureNext();
            bool first = true;
            auto submit = [&] {
                lr::render::FrameDesc frame;
                if (first) {
                    first = false;
                    frame.delta.meshes = {{lr::core::MeshId{0}, std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({0.5f, 0.5f}))}};
                    lr::core::MaterialDesc material; material.baseColor = {0, 0, 0, 1}; material.emissive = {0.8f, 0.1f, 0.05f};
                    frame.delta.materials = {{lr::core::MaterialId{0}, material}};
                    frame.delta.instances = {{lr::core::InstanceId{0}, lr::core::MeshId{0}, lr::core::MaterialId{0}, lr::core::mat4{1}}};
                    frame.delta.environment = lr::core::EnvironmentDesc{lr::core::vec3{0.05f, 0.1f, 0.4f}};
                }
                lr::render::ViewDesc view;
                view.id = lr::core::ViewId{0}; view.target = *target;
                view.camera.position = {0, 0, 3}; view.camera.target = {0, 0, 0};
                view.pipeline = renderer->pipelines().visibility();
                view.settings.toneMapping = lr::render::ToneMapping::Clamp;
                frame.views.push_back(view);
                pending.push_back(renderer->submit(std::move(frame))); ++submitted;
                maxInFlight = std::max(maxInFlight, submitted - completed);
            };
            submit(); drain(); // first with exactly one outstanding frame
            presenter->flush();
            require(capture.wait_for(3s) == std::future_status::ready, "Synchronization: capture timed out");
            auto image = capture.get();
            require(image.extent.x > 0 && image.extent.y > 0 && !image.pixels.empty(), "Synchronization: empty actual presented capture");
            auto center = image.pixels.at((image.extent.y / 2) * image.extent.x + image.extent.x / 2);
            auto corner = image.pixels.front();
            require((center & 255) > ((center >> 16) & 255) && ((corner >> 16) & 255) > (corner & 255),
                    "Synchronization: presented red-quad/blue-background capture mismatch");
            std::cout << "capture=" << image.extent.x << 'x' << image.extent.y << " centerRGBA=" << std::hex << center
                      << " cornerRGBA=" << corner << std::dec << " imageVerified=1\n";
            int transitionRemaining = 0;
            for (int i = 1; i < frames; ++i) {
                std::optional<std::future<lr::render::ImageRgba8>> transitionCapture;
                if (i == frames / 3 || i == frames / 2 || i == (2 * frames) / 3) {
                    drain(); presenter->flush();
                    auto before = submitted;
                    if (i == frames / 3) snap = guiCall(app, *factory, 1);
                    else if (i == frames / 2) {
                        guiCall(app, *factory, 2);
                        awaitExposure(false);
                        std::this_thread::sleep_for(30ms); // observation window, no submissions while hidden
                        require(submitted == before, "Synchronization: submitted while hidden");
                        require(!guiCall(app, *factory).exposed, "Platform: hidden native host remained exposed");
                        guiCall(app, *factory, 3);
                        snap = awaitExposure(true);
                        std::cout << "hideResume=1 pausedSubmissions=0\n";
                    } else {
                        auto zero = guiCall(app, *factory, 4);
                        require(zero.pixels.isEmpty(), "Platform: zero-size viewport was not paused");
                        awaitExposure(false);
                        std::this_thread::sleep_for(30ms);
                        require(submitted == before, "Synchronization: submitted during zero-size pause");
                        require(!guiCall(app, *factory).exposed, "Platform: zero-size native host remained exposed");
                        guiCall(app, *factory, 5);
                        snap = awaitExposure(true);
                        std::cout << "zeroSizePause=1 pausedSubmissions=0\n";
                    }
                    require(snap.exposed && !snap.pixels.isEmpty(), "Platform: restored viewport is not exposed or has zero area");
                    renderer->targets().resize(*target, {snap.pixels.width(), snap.pixels.height()});
                    std::cout << "resizedPhysical=" << snap.pixels.width() << 'x' << snap.pixels.height() << " DPR=" << snap.dpr << '\n';
                    require(transitionRemaining == 0, "Platform: previous resize did not settle before next transition");
                    transitionRemaining = 3;
                }
                if (transitionRemaining) transitionCapture.emplace(presenter->captureNext());
                submit();
                if (transitionCapture) {
                    drain(); presenter->flush();
                    require(transitionCapture->wait_for(3s) == std::future_status::ready,
                            "Synchronization: post-transition presented capture timed out");
                    auto displayed = transitionCapture->get();
                    std::cout << "transitionPresented=" << displayed.extent.x << 'x' << displayed.extent.y
                              << " expected=" << snap.pixels.width() << 'x' << snap.pixels.height()
                              << " settleAttempt=" << 4 - transitionRemaining << '\n';
                    // XCB WSI may return a valid image of the OLD swapchain once, then
                    // report SUBOPTIMAL on acquire/present; upstream recreates next frame.
                    // Require actual convergence within three presented frames, not an
                    // unsupported promise of immediate first-frame swapchain replacement.
                    if (displayed.extent.x == snap.pixels.width() && displayed.extent.y == snap.pixels.height())
                        transitionRemaining = 0;
                    else require(--transitionRemaining > 0, "Platform: presented extent failed to converge after resize");
                } else if (pending.size() == 2) drain();
            }
            drain();
            require(transitionRemaining == 0, "Platform: final transition was not verified");
            require(maxInFlight == 2, "Synchronization: two-frame outstanding case was not exercised");
            code = 0;
        } catch (std::exception const &error) {
            std::cerr << "probe failure: " << error.what() << '\n';
            code = (!deviceAvailable && !requireShared) ? 77 : 1;
        }
        try { cleanup(); }
        catch (std::exception const &error) {
            // Do not destroy a GUI native host underneath an uncertain live GPU owner.
            std::cerr << "Synchronization: cleanup failed: " << error.what() << '\n';
            std::_Exit(2);
        }
        QMetaObject::invokeMethod(&app, [&] {
            factory->releaseHost();
            std::cout << "guiHeartbeats=" << heartbeat.load() << " maxHeartbeatGapMs=" << maxGap
                      << " hostReleasedAfterPresenter=1\n";
            if (code == 0 && (heartbeat.load() < 2 || maxGap > 500)) { std::cerr << "Platform: GUI responsiveness budget failed\n"; code = 1; }
            app.exit(code);
        }, Qt::QueuedConnection);
    });
    app.exec();
    worker.join();
    { std::lock_guard<std::mutex> lock(watchdogMutex); finished = true; }
    watchdogWake.notify_all(); watchdog.join();
    return code;
}
