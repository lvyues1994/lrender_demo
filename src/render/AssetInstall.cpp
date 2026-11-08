#include "AssetInstall.h"
#include <QCoreApplication>
#include <QThread>
#include <cmath>
#include <stdexcept>
namespace demo {
struct AssetInstall::Impl {
  lr::scene::World &world;
  QThread *owner = QThread::currentThread();
  std::optional<float> roughness;
  bool normalMapping = true;
  struct Bundle {
    CpuAssetPtr asset;
    std::size_t next = 0;
    std::vector<lr::core::EntityId> entities;
    std::vector<lr::core::MeshId> meshes;
    std::vector<lr::core::MaterialId> materials;
    std::vector<lr::core::TextureId> textures;
    std::vector<std::optional<lr::core::MaterialId>> materialMap;
    std::vector<std::optional<lr::core::TextureId>> textureMap;
  } installed, staged;
  explicit Impl(lr::scene::World &w) : world(w) {
    if (!QCoreApplication::instance() ||
        owner != QCoreApplication::instance()->thread())
      throw std::logic_error("AssetInstall requires GUI World owner");
  }
  void check() const {
    if (QThread::currentThread() != owner)
      throw std::logic_error("AssetInstall requires World GUI owner");
  }
  void release(Bundle &b) {
    for (auto id : b.entities)
      world.destroyEntity(id);
    for (auto id : b.meshes)
      world.resources().releaseMesh(id);
    for (auto id : b.materials)
      world.resources().releaseMaterial(id);
    for (auto id : b.textures)
      world.resources().releaseTexture(id);
    b = {};
  }
  std::optional<lr::core::TextureId> texture(std::optional<std::size_t> index) {
    if (!index)
      return {};
    auto &id = staged.textureMap[*index];
    if (!id) {
      auto const &t = staged.asset->textures[*index];
      auto added = world.resources().addTexture(t.image, t.sampler);
      staged.textures.push_back(added);
      id = added;
    }
    return id;
  }
  lr::core::MaterialId material(std::size_t index) {
    auto &id = staged.materialMap[index];
    if (!id) {
      auto const &m = staged.asset->materials[index];
      auto d = m.factors;
      if (roughness)
        d.roughness = *roughness;
      if (!normalMapping)
        d.normalScale = 0;
      d.baseColorTexture = texture(m.baseColorTexture);
      d.normalTexture = texture(m.normalTexture);
      d.emissiveTexture = texture(m.emissiveTexture);
      auto added = world.resources().addMaterial(d);
      staged.materials.push_back(added);
      id = added;
    }
    return *id;
  }
};
AssetInstall::AssetInstall(lr::scene::World &w)
    : impl_(std::make_unique<Impl>(w)) {}
AssetInstall::~AssetInstall() { clear(); }
void AssetInstall::setNormalMapping(bool enabled) {
  auto &s = *impl_;
  s.check();
  s.normalMapping = enabled;
  auto update = [&](auto &b) {
    if (!b.asset)
      return;
    for (std::size_t i = 0; i < b.materialMap.size(); ++i)
      if (b.materialMap[i]) {
        auto const &m = b.asset->materials[i];
        auto d = m.factors;
        if (s.roughness)
          d.roughness = *s.roughness;
        if (!enabled)
          d.normalScale = 0;
        auto texture = [&](auto index) -> std::optional<lr::core::TextureId> {
          return index ? b.textureMap[*index] : std::nullopt;
        };
        d.baseColorTexture = texture(m.baseColorTexture);
        d.normalTexture = texture(m.normalTexture);
        d.emissiveTexture = texture(m.emissiveTexture);
        s.world.resources().replaceMaterial(*b.materialMap[i], d);
      }
  };
  update(s.installed);
  update(s.staged);
}
void AssetInstall::setRoughness(float value) {
  auto &s = *impl_;
  s.check();
  if (!std::isfinite(value) || value < 0 || value > 1)
    throw std::invalid_argument("roughness requires finite [0,1]");
  s.roughness = value;
  auto update = [&](auto &bundle) {
    if (!bundle.asset)
      return;
    for (std::size_t i = 0; i < bundle.materialMap.size(); ++i)
      if (bundle.materialMap[i]) {
        auto const &m = bundle.asset->materials[i];
        auto d = m.factors;
        d.roughness = value;
        if (!s.normalMapping)
          d.normalScale = 0;
        auto texture = [&](auto index) -> std::optional<lr::core::TextureId> {
          return index ? bundle.textureMap[*index] : std::nullopt;
        };
        d.baseColorTexture = texture(m.baseColorTexture);
        d.normalTexture = texture(m.normalTexture);
        d.emissiveTexture = texture(m.emissiveTexture);
        s.world.resources().replaceMaterial(*bundle.materialMap[i], d);
      }
  };
  update(s.installed);
  update(s.staged);
}
void AssetInstall::begin(CpuAssetPtr a) {
  auto &s = *impl_;
  s.check();
  if (s.staged.asset)
    throw std::logic_error("installation already active");
  if (!a || a->parts.empty())
    throw std::invalid_argument("empty CPU asset");
  for (auto const &p : a->parts)
    if (!p.mesh || p.material >= a->materials.size())
      throw std::invalid_argument("invalid CPU part");
  for (auto const &m : a->materials) {
    auto const &f = m.factors;
    if (f.baseColorTexture || f.normalTexture || f.emissiveTexture ||
        f.metallicRoughnessTexture)
      throw std::invalid_argument("CPU factors contain foreign World IDs");
    auto validate = [&](auto i, TextureRole role) {
      if (i && (*i >= a->textures.size() || !a->textures[*i].image ||
                a->textures[*i].role != role))
        throw std::invalid_argument("invalid CPU texture reference/role");
    };
    validate(m.baseColorTexture, TextureRole::Diffuse);
    validate(m.normalTexture, TextureRole::Normal);
    validate(m.emissiveTexture, TextureRole::Emissive);
  }
  Impl::Bundle b;
  b.asset = std::move(a);
  b.entities.reserve(b.asset->parts.size());
  b.meshes.reserve(b.asset->parts.size());
  b.materials.reserve(b.asset->materials.size());
  b.textures.reserve(b.asset->textures.size());
  b.materialMap.resize(b.asset->materials.size());
  b.textureMap.resize(b.asset->textures.size());
  s.staged = std::move(b);
}
bool AssetInstall::step(std::size_t maxParts) {
  auto &s = *impl_;
  s.check();
  if (!s.staged.asset || !maxParts)
    throw std::logic_error(
        "step requires active transaction and positive budget");
  auto &b = s.staged;
  try {
    for (std::size_t n = 0; n < maxParts && b.next < b.asset->parts.size();
         ++n) {
      auto const &p = b.asset->parts[b.next];
      auto material = s.material(p.material);
      auto mesh = s.world.resources().addMesh(p.mesh);
      b.meshes.push_back(mesh);
      auto entity = s.world.createEntity({}, std::nullopt);
      b.entities.push_back(entity);
      s.world.setRenderable(entity, {mesh, material});
      ++b.next;
    }
  } catch (...) {
    rollback();
    throw;
  }
  return b.next == b.asset->parts.size();
}
void AssetInstall::commit() {
  auto &s = *impl_;
  s.check();
  if (!s.staged.asset || s.staged.next != s.staged.asset->parts.size())
    throw std::logic_error("cannot commit incomplete installation");
  s.release(s.installed);
  s.installed = std::move(s.staged);
  s.staged = {};
}
void AssetInstall::rollback() {
  impl_->check();
  impl_->release(impl_->staged);
}
void AssetInstall::clear() {
  impl_->check();
  impl_->release(impl_->staged);
  impl_->release(impl_->installed);
}
bool AssetInstall::active() const {
  impl_->check();
  return bool(impl_->staged.asset);
}
std::size_t AssetInstall::completedParts() const {
  impl_->check();
  return impl_->staged.next;
}
CpuAssetPtr AssetInstall::current() const {
  impl_->check();
  return impl_->installed.asset;
}
} // namespace demo
