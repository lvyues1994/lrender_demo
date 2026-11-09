#include "editor/GizmoMath.h"

#include <tc/mat.hpp>

#include <algorithm>
#include <limits>

namespace demo::editor {
namespace {

lr::core::vec3 normalizeSafe(lr::core::vec3 v) noexcept {
  float len = tc::length(v);
  if (len < 1e-8f)
    return {0, 1, 0};
  return v / len;
}

float saturate(float x) noexcept { return std::clamp(x, 0.f, 1.f); }

float distPointSegment2D(lr::core::vec2 p, lr::core::vec2 a, lr::core::vec2 b) noexcept {
  lr::core::vec2 ab = b - a;
  float denom = ab.x * ab.x + ab.y * ab.y;
  float t = 0.f;
  if (denom > 1e-8f)
    t = saturate(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / denom);
  lr::core::vec2 c{a.x + ab.x * t, a.y + ab.y * t};
  float dx = p.x - c.x, dy = p.y - c.y;
  return std::sqrt(dx * dx + dy * dy);
}

float axisParamOnRay(Ray const &ray, lr::core::vec3 const &origin,
                     lr::core::vec3 const &axis) noexcept {
  lr::core::vec3 d1 = ray.direction;
  lr::core::vec3 d2 = axis;
  lr::core::vec3 r = origin - ray.origin;
  float a = tc::dot(d1, d1);
  float b = tc::dot(d1, d2);
  float c = tc::dot(d2, d2);
  float d = tc::dot(d1, r);
  float e = tc::dot(d2, r);
  float denom = a * c - b * b;
  if (std::fabs(denom) > 1e-8f)
    return (a * e - b * d) / denom;
  if (std::fabs(c) > 1e-8f)
    return e / c;
  return 0.f;
}

} // namespace

Ray cameraRay(lr::core::CameraDesc const &camera, ViewportExtent const &extent,
              float pixelX, float pixelY) noexcept {
  float w = std::max(1.f, float(extent.logical.x));
  float h = std::max(1.f, float(extent.logical.y));
  float ndcX = (2.f * pixelX / w) - 1.f;
  float ndcY = 1.f - (2.f * pixelY / h);
  float aspect = w / h;
  lr::core::vec3 forward = normalizeSafe(camera.target - camera.position);
  lr::core::vec3 right = normalizeSafe(tc::cross(forward, camera.up));
  lr::core::vec3 up = tc::cross(right, forward);
  float tanHalf = std::tan(camera.verticalFov * 0.5f);
  lr::core::vec3 dir =
      normalizeSafe(forward + right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf));
  return Ray{camera.position, dir};
}

std::optional<float> intersectAabb(Ray const &ray, lr::core::Aabb const &box) noexcept {
  float tmin = 0.f;
  float tmax = std::numeric_limits<float>::infinity();
  float const mins[3] = {box.min.x, box.min.y, box.min.z};
  float const maxs[3] = {box.max.x, box.max.y, box.max.z};
  float const o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  float const d[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(d[i]) < 1e-8f) {
      if (o[i] < mins[i] || o[i] > maxs[i])
        return std::nullopt;
      continue;
    }
    float inv = 1.f / d[i];
    float t1 = (mins[i] - o[i]) * inv;
    float t2 = (maxs[i] - o[i]) * inv;
    if (t1 > t2)
      std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmax < tmin)
      return std::nullopt;
  }
  if (tmax < 0.f)
    return std::nullopt;
  return tmin >= 0.f ? tmin : tmax;
}

std::optional<RayHit> pickClosest(
    Ray const &ray,
    std::vector<std::pair<EditorEntityId, lr::core::Aabb>> const &boxes) noexcept {
  std::optional<RayHit> best;
  for (auto const &b : boxes) {
    auto t = intersectAabb(ray, b.second);
    if (!t)
      continue;
    if (!best || *t < best->t)
      best = RayHit{*t, b.first};
  }
  return best;
}

std::optional<lr::core::vec2> projectToPixel(lr::core::CameraDesc const &camera,
                                            ViewportExtent const &extent,
                                            lr::core::vec3 const &world) noexcept {
  float w = std::max(1.f, float(extent.logical.x));
  float h = std::max(1.f, float(extent.logical.y));
  float aspect = w / h;
  auto view = lr::core::viewMatrix(camera);
  auto proj = lr::core::projectionMatrix(camera, aspect);
  lr::core::vec4 clip = proj * (view * lr::core::vec4{world.x, world.y, world.z, 1.f});
  if (std::fabs(clip.w) < 1e-8f)
    return std::nullopt;
  float ndcX = clip.x / clip.w;
  float ndcY = clip.y / clip.w;
  if (ndcX < -1.f || ndcX > 1.f || ndcY < -1.f || ndcY > 1.f)
    return std::nullopt;
  return lr::core::vec2{(ndcX + 1.f) * 0.5f * w, (1.f - ndcY) * 0.5f * h};
}

GizmoHandleHit hitTranslateHandle(Ray const &ray, lr::core::vec3 const &origin,
                                  lr::core::vec3 const &axisDir, float axisLength,
                                  lr::core::CameraDesc const &camera, ViewportExtent const &extent,
                                  float pixelThreshold) noexcept {
  auto axis = normalizeSafe(axisDir);
  float tAxis = std::clamp(axisParamOnRay(ray, origin, axis), 0.f, axisLength);
  // Closest point on ray (t >= 0) to that axis point.
  lr::core::vec3 axisPt = origin + axis * tAxis;
  lr::core::vec3 ro = axisPt - ray.origin;
  float tRay = tc::dot(ro, ray.direction);
  if (tRay < 0.f)
    tRay = 0.f;
  lr::core::vec3 rayPt = ray.origin + ray.direction * tRay;
  // Compare in pixels via projection of the two closest points.
  auto pixRay = projectToPixel(camera, extent, rayPt);
  auto pixAxis = projectToPixel(camera, extent, axisPt);
  auto p0 = projectToPixel(camera, extent, origin);
  auto p1 = projectToPixel(camera, extent, origin + axis * axisLength);
  if (!pixRay || !pixAxis || !p0 || !p1)
    return {};
  float dx = pixRay->x - pixAxis->x, dy = pixRay->y - pixAxis->y;
  float dist = std::sqrt(dx * dx + dy * dy);
  dist = std::min(dist, distPointSegment2D(*pixRay, *p0, *p1));
  if (dist > pixelThreshold)
    return {};
  GizmoHandleHit hit;
  hit.t = tRay;
  hit.axis = GizmoAxis::X;
  return hit;
}

lr::core::vec3 translateDeltaAlongAxis(Ray const &ray0, Ray const &ray1,
                                       lr::core::vec3 const &origin,
                                       lr::core::vec3 const &axisDir) noexcept {
  auto axis = normalizeSafe(axisDir);
  float t0 = axisParamOnRay(ray0, origin, axis);
  float t1 = axisParamOnRay(ray1, origin, axis);
  return axis * (t1 - t0);
}

lr::core::mat4 localToWorld(lr::core::Transform const &local,
                            lr::core::mat4 const &parentWorld) noexcept {
  return parentWorld * lr::core::matrixOf(local);
}

lr::core::Transform worldToLocal(lr::core::Transform const &worldDesired,
                                 lr::core::mat4 const &parentWorld) noexcept {
  auto worldMat = lr::core::matrixOf(worldDesired);
  auto localMat = tc::inverse(parentWorld) * worldMat;
  return lr::core::decomposed(localMat);
}

lr::core::Transform applyWorldTranslationDelta(lr::core::Transform const &local,
                                               lr::core::mat4 const &parentWorld,
                                               lr::core::vec3 const &worldDelta) noexcept {
  auto worldMat = localToWorld(local, parentWorld);
  lr::core::Transform worldXf = lr::core::decomposed(worldMat);
  worldXf.translation = worldXf.translation + worldDelta;
  auto solved = worldToLocal(worldXf, parentWorld);
  lr::core::Transform out = local;
  out.translation = solved.translation;
  return out;
}

} // namespace demo::editor
