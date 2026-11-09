#include "editor/GizmoVisuals.h"

#include <lr/core/Mesh.h>
#include <lr/core/Primitives.h>
#include <lr/core/Scene.h>

#include <cmath>

namespace demo::editor {
namespace {

lr::core::MaterialDesc emissiveColor(lr::core::vec3 rgb) {
  lr::core::MaterialDesc d{};
  d.baseColor = {rgb.x, rgb.y, rgb.z, 1.f};
  d.emissive = rgb * 4.f;
  d.roughness = 1.f;
  d.metallic = 0.f;
  d.doubleSided = true;
  return d;
}

lr::core::MeshData makeRingMesh(float radius, float tube, int segments) {
  // Flat annulus in XY (about Z); oriented via entity transform.
  lr::core::MeshData mesh;
  mesh.positions.reserve(std::size_t(segments) * 2);
  mesh.normals.reserve(std::size_t(segments) * 2);
  mesh.indices.reserve(std::size_t(segments) * 6);
  float const r0 = radius - tube * 0.5f;
  float const r1 = radius + tube * 0.5f;
  for (int i = 0; i < segments; ++i) {
    float a0 = float(i) / float(segments) * lr::core::kPi * 2.f;
    float c = std::cos(a0), s = std::sin(a0);
    mesh.positions.push_back({r0 * c, r0 * s, 0.f});
    mesh.positions.push_back({r1 * c, r1 * s, 0.f});
    mesh.normals.push_back({0, 0, 1});
    mesh.normals.push_back({0, 0, 1});
  }
  for (int i = 0; i < segments; ++i) {
    std::uint32_t i0 = std::uint32_t(i * 2);
    std::uint32_t i1 = i0 + 1;
    std::uint32_t j0 = std::uint32_t(((i + 1) % segments) * 2);
    std::uint32_t j1 = j0 + 1;
    mesh.indices.push_back(i0);
    mesh.indices.push_back(j0);
    mesh.indices.push_back(i1);
    mesh.indices.push_back(i1);
    mesh.indices.push_back(j0);
    mesh.indices.push_back(j1);
  }
  return mesh;
}

lr::core::vec3 basisAxis(lr::core::mat4 const &m, GizmoAxis axis, GizmoSpace space) {
  if (space == GizmoSpace::World) {
    if (axis == GizmoAxis::Y)
      return {0, 1, 0};
    if (axis == GizmoAxis::Z)
      return {0, 0, 1};
    return {1, 0, 0};
  }
  if (axis == GizmoAxis::X)
    return tc::normalize(lr::core::vec3{m[0][0], m[0][1], m[0][2]});
  if (axis == GizmoAxis::Y)
    return tc::normalize(lr::core::vec3{m[1][0], m[1][1], m[1][2]});
  return tc::normalize(lr::core::vec3{m[2][0], m[2][1], m[2][2]});
}

lr::core::Transform axisAligned(lr::core::vec3 origin, lr::core::vec3 dir, lr::core::vec3 scale) {
  lr::core::Transform t = lr::core::Transform::identity();
  t.translation = origin;
  t.scale = scale;
  // Rotate +Y (box default long axis we'll use as shaft along Y) toward dir... 
  // Our shaft mesh is elongated on X; rotate +X toward dir.
  lr::core::vec3 x = tc::normalize(dir);
  lr::core::vec3 up = std::fabs(x.y) < 0.9f ? lr::core::vec3{0, 1, 0} : lr::core::vec3{1, 0, 0};
  lr::core::vec3 z = tc::normalize(tc::cross(x, up));
  lr::core::vec3 y = tc::cross(z, x);
  lr::core::mat3 R{};
  R[0][0] = x.x; R[0][1] = x.y; R[0][2] = x.z;
  R[1][0] = y.x; R[1][1] = y.y; R[1][2] = y.z;
  R[2][0] = z.x; R[2][1] = z.y; R[2][2] = z.z;
  t.rotation = lr::core::Rotation::fromMatrix(R);
  return t;
}

lr::core::Transform ringOriented(lr::core::vec3 origin, lr::core::vec3 axisNormal) {
  // Ring mesh lies in XY; align its +Z with axisNormal.
  return axisAligned(origin, axisNormal, {1, 1, 1}); // wait - axisAligned maps +X to dir
}
} // namespace

void GizmoVisuals::clear(lr::scene::World &world) {
  for (auto id : entities_)
    if (id.isValid() && world.isAlive(id))
      world.destroyEntity(id);
  entities_.clear();
  for (auto id : ownedMeshes_)
    if (id.isValid())
      world.resources().releaseMesh(id);
  ownedMeshes_.clear();
  for (auto id : ownedMaterials_)
    if (id.isValid())
      world.resources().releaseMaterial(id);
  ownedMaterials_.clear();
  shaftMesh_ = tipMesh_ = ringMesh_ = centerMesh_ = {};
  matR_ = matG_ = matB_ = matY_ = {};
  resourcesReady_ = false;
}

void GizmoVisuals::ensureResources(lr::scene::World &world) {
  if (resourcesReady_)
    return;
  auto addMesh = [&](lr::core::MeshData data) {
    auto handle = std::make_shared<lr::core::MeshData const>(std::move(data));
    auto id = world.resources().addMesh(handle);
    ownedMeshes_.push_back(id);
    return id;
  };
  auto addMat = [&](lr::core::vec3 c) {
    auto id = world.resources().addMaterial(emissiveColor(c));
    ownedMaterials_.push_back(id);
    return id;
  };
  // Shaft: thin box along +X, length 1.25 centered at 0.625 on X.
  shaftMesh_ = addMesh(lr::core::makeBox({0.625f, 0.03f, 0.03f}));
  tipMesh_ = addMesh(lr::core::makeBox({0.06f, 0.06f, 0.06f}));
  ringMesh_ = addMesh(makeRingMesh(1.0f, 0.04f, 48));
  centerMesh_ = addMesh(lr::core::makeBox({0.08f, 0.08f, 0.08f}));
  matR_ = addMat({1.f, 0.15f, 0.1f});
  matG_ = addMat({0.15f, 1.f, 0.2f});
  matB_ = addMat({0.2f, 0.35f, 1.f});
  matY_ = addMat({1.f, 0.9f, 0.2f});
  resourcesReady_ = true;
}

void GizmoVisuals::rebuild(lr::scene::World &world, lr::core::mat4 const &entityWorld, GizmoMode mode,
                           GizmoSpace space) {
  for (auto id : entities_)
    if (id.isValid() && world.isAlive(id))
      world.destroyEntity(id);
  entities_.clear();
  ensureResources(world);
  lr::core::vec3 origin{entityWorld[3][0], entityWorld[3][1], entityWorld[3][2]};
  lr::core::MaterialId mats[3] = {matR_, matG_, matB_};
  GizmoAxis axes[3] = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};

  auto spawn = [&](lr::core::MeshId mesh, lr::core::MaterialId mat, lr::core::Transform local) {
    auto e = world.createEntity(local, std::nullopt);
    world.setRenderable(e, {mesh, mat});
    entities_.push_back(e);
  };

  if (mode == GizmoMode::Rotate) {
    for (int i = 0; i < 3; ++i) {
      auto n = basisAxis(entityWorld, axes[i], space);
      // Ring in plane ⊥ n: map ring +Z → n via lookingAlong style.
      lr::core::Transform t = lr::core::Transform::identity();
      t.translation = origin;
      t.rotation = lr::core::Rotation::lookingAlong(-n, // lookingAlong turns -z toward forward
                                                    std::fabs(n.y) < 0.9f ? lr::core::vec3{0, 1, 0}
                                                                         : lr::core::vec3{1, 0, 0});
      spawn(ringMesh_, mats[i], t);
    }
    return;
  }

  for (int i = 0; i < 3; ++i) {
    auto dir = basisAxis(entityWorld, axes[i], space);
    // Shaft centered halfway along handle.
    auto shaftT = axisAligned(origin + dir * 0.625f, dir, {1, 1, 1});
    // makeBox half-extents 0.625 on X → full length 1.25 along dir after axisAligned.
    spawn(shaftMesh_, mats[i], shaftT);
    auto tipT = axisAligned(origin + dir * 1.25f, dir, {1, 1, 1});
    spawn(tipMesh_, mats[i], tipT);
  }
  if (mode == GizmoMode::Scale) {
    lr::core::Transform c = lr::core::Transform::identity();
    c.translation = origin;
    spawn(centerMesh_, matY_, c);
  }
}

void GizmoVisuals::sync(lr::scene::World &world, lr::core::mat4 const &entityWorld, GizmoMode mode,
                        GizmoSpace space) {
  // Always rebuild — entity count/mode may change; cheap for a handful of helpers.
  rebuild(world, entityWorld, mode, space);
  lastMode_ = mode;
}

} // namespace demo::editor
