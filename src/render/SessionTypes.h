#pragma once
#include <lr/core/Scene.h>
#include <lr/render/Renderer.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
namespace demo {
struct ModelId { std::string value; };
using AssetRequestId = std::uint64_t;
using SessionGeneration = std::uint64_t;
enum class PipelineKind { Visibility, Pathtrace };
enum class PresentationPath { VulkanShared, Readback };
enum class LifeState { Idle, Preparing, Running, Paused, Recovering, Stopping, Stopped, Failed };
enum class ErrorStage { Load, Surface, Probe, Submit, Present, Shutdown };
struct RenderError { ErrorStage stage; bool recoverable; std::string message, cause; };
template<class T> using Result = std::variant<T, RenderError>;
struct ViewportExtent { lr::core::ivec2 logical{0}, physical{0}; double dpr = 1; };
struct CameraState { lr::core::CameraDesc camera; };
struct RenderOptions {
    PipelineKind pipeline = PipelineKind::Visibility;
    float exposure = 1;
    bool taa = false;
};
struct FrameStamp {
    lr::core::ViewId view;
    SessionGeneration generation = 0;
    std::uint64_t sequence = 0;
};
struct OwnedFrame { FrameStamp stamp; std::shared_ptr<lr::render::ImageRgba8 const> image; };
struct PerformanceSnapshot {
    std::uint32_t inFlight = 0;
    std::uint32_t accumulatedSamples = 0; // actual latest accepted ViewStats, not fake GPU timing
    std::uint64_t deltaExtractions = 0; // successful real World::extractDelta calls
    std::uint64_t completed = 0, displayed = 0, dropped = 0;
    double windowSeconds = 0;
    std::optional<double> hostPrepareMs, frameWallMs, presenterLatencyMs, gpuPassTotalMs;
};
struct SessionSnapshot {
    LifeState life = LifeState::Idle;
    PresentationPath path = PresentationPath::Readback;
    SessionGeneration generation = 0;
    std::optional<ModelId> model; // desired/requested model, not proof of World installation
    AssetRequestId request = 0;
    CameraState camera;
    RenderOptions options;
    PerformanceSnapshot performance;
    std::optional<RenderError> error; // last reported issue; life identifies active/terminal failure
    std::string backend, device;
    std::vector<std::string> warnings;
};
using SessionSnapshotPtr = std::shared_ptr<SessionSnapshot const>;
using Done = std::function<void(Result<std::monostate>)>;
struct ProbeReport { bool shared = false; std::string reason, device; };
using TargetDesc = std::variant<lr::render::ImageTargetDesc, lr::render::WindowTargetDesc>;
struct PresentationEvents { std::function<void(OwnedFrame)> onImage; std::function<void(RenderError)> onError; };
struct FrameCompletion { FrameStamp stamp; Result<lr::render::FrameResult> result; };
using SubmitFrame = std::function<void(lr::render::FrameDesc, FrameStamp, std::function<void(FrameCompletion)>)>;
struct SessionEvents {
    std::function<void(SessionSnapshotPtr)> onSnapshot;
    std::function<void(RenderError)> onError;
    std::function<void()> onStopped;
};
} // namespace demo
