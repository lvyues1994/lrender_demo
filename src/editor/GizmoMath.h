#pragma once

#include "render/SessionTypes.h"

#include <lr/core/Math.h>
#include <lr/core/Scene.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace demo::editor {

struct Ray {
  lr::core::vec3 origin{};
  lr::core::vec3 direction{}; // normalized
};

struct RayHit {
  float t = 0.f;
  EditorEntityId id{};
};

// Pixel center in logical widget coords → world ray (OpenGL-style NDC, Y up).
Ray cameraRay(lr::core::CameraDesc const &camera, ViewportExtent const &extent,
              float pixelX, float pixelY) noexcept;

// Slab test; returns entry t or nullopt. Infinite miss when no intersection.
std::optional<float> intersectAabb(Ray const &ray, lr::core::Aabb const &box) noexcept;

// Closest hit among candidates (world-space AABBs). Ties keep first.
std::optional<RayHit> pickClosest(Ray const &ray,
                                  std::vector<std::pair<EditorEntityId, lr::core::Aabb>> const &boxes) noexcept;

enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoSpace { Local, World };
enum class GizmoAxis { None, X, Y, Z, Screen };

struct GizmoHandleHit {
  GizmoAxis axis = GizmoAxis::None;
  float t = 0.f;
};

// Project world point to logical pixel (same convention as cameraRay).
std::optional<lr::core::vec2> projectToPixel(lr::core::CameraDesc const &camera,
                                            ViewportExtent const &extent,
                                            lr::core::vec3 const &world) noexcept;

// Axis handle as a short segment from origin along axisDir (unit), length in world units.
// Hit if ray comes within pixelThreshold of the projected segment.
GizmoHandleHit hitTranslateHandle(Ray const &ray, lr::core::vec3 const &origin,
                                  lr::core::vec3 const &axisDir, float axisLength,
                                  lr::core::CameraDesc const &camera, ViewportExtent const &extent,
                                  float pixelThreshold) noexcept;

// Drag: project mouse ray onto axis through origin; return world delta along axis.
lr::core::vec3 translateDeltaAlongAxis(Ray const &ray0, Ray const &ray1,
                                       lr::core::vec3 const &origin,
                                       lr::core::vec3 const &axisDir) noexcept;

// Compose world matrix from local TRS + optional parent world matrix.
lr::core::mat4 localToWorld(lr::core::Transform const &local,
                            lr::core::mat4 const &parentWorld) noexcept;

// Given desired world transform and parent world, solve local TRS (translation-focused;
// rotation/scale copied from desiredWorld when parent is identity-ish — see tests).
lr::core::Transform worldToLocal(lr::core::Transform const &worldDesired,
                                 lr::core::mat4 const &parentWorld) noexcept;

lr::core::Transform applyWorldTranslationDelta(lr::core::Transform const &local,
                                               lr::core::mat4 const &parentWorld,
                                               lr::core::vec3 const &worldDelta) noexcept;

// Minimum positive scale component after gizmo scale (never 0 / negative).
inline constexpr float kMinScale = 1e-3f;

// Signed angle (radians) about axis from ray0∩plane to ray1∩plane. nullopt if either misses.
std::optional<float> rotateAngleAroundAxis(Ray const &ray0, Ray const &ray1,
                                           lr::core::vec3 const &origin,
                                           lr::core::vec3 const &axisDir) noexcept;

// Apply world-space rotation delta about axis through the entity origin; solve back to local.
lr::core::Transform applyWorldRotationDelta(lr::core::Transform const &local,
                                            lr::core::mat4 const &parentWorld,
                                            lr::core::vec3 const &axisDir,
                                            float radians) noexcept;

// Scale factor = axisParam(ray1)/axisParam(ray0); 1 if press param ~0.
float scaleFactorAlongAxis(Ray const &ray0, Ray const &ray1,
                           lr::core::vec3 const &origin,
                           lr::core::vec3 const &axisDir) noexcept;

// Apply scale factor on one axis (or uniform); clamps each component to >= kMinScale.
lr::core::Transform applyScaleDelta(lr::core::Transform const &local,
                                    lr::core::mat4 const &parentWorld,
                                    lr::core::vec3 const &axisDir,
                                    float factor, bool uniform) noexcept;

// Rotate ring: ray near circle in plane ⊥ axisDir, radius ringRadius (pixel threshold).
GizmoHandleHit hitRotateHandle(Ray const &ray, lr::core::vec3 const &origin,
                               lr::core::vec3 const &axisDir, float ringRadius,
                               lr::core::CameraDesc const &camera, ViewportExtent const &extent,
                               float pixelThreshold) noexcept;

// Scale handle reuses translate-style segment hit (tip); Screen axis = uniform center.
GizmoHandleHit hitScaleHandle(Ray const &ray, lr::core::vec3 const &origin,
                              lr::core::vec3 const &axisDir, float axisLength,
                              lr::core::CameraDesc const &camera, ViewportExtent const &extent,
                              float pixelThreshold) noexcept;

} // namespace demo::editor
