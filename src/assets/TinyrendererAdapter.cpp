#include "TinyrendererAdapter.h"
#include "ObjLoader.h"
#include "TgaDecoder.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace demo {
namespace {
void checkpoint(std::function<bool()> const &stop) {
  if (stop && stop())
    throw std::runtime_error("cancelled");
}
std::filesystem::path checkedPath(std::string const &relative) {
  namespace fs = std::filesystem;
  fs::path p(relative);
  if (p.empty() || p.is_absolute())
    throw std::runtime_error("asset path must be relative");
  for (auto const &x : p)
    if (x == "..")
      throw std::runtime_error("asset path escape rejected");
  auto root = fs::canonical(DEMO_TINYRENDERER_ROOT), resolved = fs::canonical(root / p);
  auto a = root.begin(), b = resolved.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == resolved.end() || *a != *b)
      throw std::runtime_error("asset symlink escape rejected");
  if (!fs::is_regular_file(resolved))
    throw std::runtime_error("asset is not a regular file");
  return resolved;
}
std::vector<std::uint8_t> read(std::string const &name, std::size_t limit,
                               std::function<bool()> const &stop) {
  checkpoint(stop);
  auto path = checkedPath(name);
  auto size = std::filesystem::file_size(path);
  if (size > limit)
    throw std::runtime_error("asset input byte limit");
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot open asset");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  for (std::size_t pos = 0; pos < bytes.size();) {
    checkpoint(stop);
    auto n = std::min<std::size_t>(65536, bytes.size() - pos);
    if (!f.read(reinterpret_cast<char *>(bytes.data() + pos), n))
      throw std::runtime_error("truncated asset read");
    pos += n;
  }
  checkpoint(stop);
  return bytes;
}
bool same(AssetPartEntry const &a, AssetPartEntry const &b) {
  return a.objPath == b.objPath && a.diffusePath == b.diffusePath && a.normalPath == b.normalPath &&
         a.emissivePath == b.emissivePath && a.specPath == b.specPath;
}
} // namespace
Result<CpuAssetPtr> decodeAsset(AssetEntry const &entry, std::function<bool()> stop) {
  try {
    checkpoint(stop);
    auto known = AssetCatalog{}.describe(entry.id);
    if (auto e = std::get_if<RenderError>(&known))
      return *e;
    auto const &canonical = std::get<AssetEntry>(known);
    if (entry.objPath != canonical.objPath || entry.licensePath != canonical.licensePath ||
        entry.parts.size() != canonical.parts.size() ||
        !std::equal(entry.parts.begin(), entry.parts.end(), canonical.parts.begin(), same))
      throw std::runtime_error("modified/untrusted catalog manifest rejected");
    (void)checkedPath(entry.licensePath);
    (void)checkedPath("LICENSE.txt");
    auto asset = std::make_shared<CpuAsset>();
    asset->model = entry.id;
    std::size_t used = 0;
    auto budget = [&](std::size_t n) {
      if (n > 128u * 1024 * 1024 - used)
        throw std::runtime_error("CPU asset exceeds 128MiB aggregate limit");
      used += n;
    };
    asset->notices = {
        "Source: https://github.com/ssloy/tinyrenderer ; original resources remain read-only.",
        "LICENSE.txt is software permission, not a blanket license for model redistribution. See " +
            entry.licensePath + "; do not redistribute without applicable permission."};
    if (entry.id.value == "african_head")
      asset->notices.push_back(
          "Vidar Rapp, African head 2007; permission as renderer example, see original readme.");
    if (entry.id.value == "diablo")
      asset->notices.push_back("Samuel (arshlevon) Sharit; original email permission in model readme, no "
                               "broader license inferred.");
    if (entry.id.value == "boggie")
      asset->notices.push_back("Bojana Nedeljkovic; educational project permission 6 February 2015; no "
                               "general commercial/redistribution grant inferred.");
    if (entry.id.value == "floor")
      asset->notices.push_back("Floor source is TinyRenderer repository; no separate model grant found. "
                               "Software LICENSE is not asserted to cover this model.");
    asset->warnings.push_back(
        "Specular/gloss maps are NOT PBR metallic. Fixed metallic=0, roughness=0.5 approximation; specular "
        "bytes retained linear but unbound, gloss unmapped. Third vt component and normalized vertex RGB "
        "extension validated/discarded like source Model (2D textured adaptation, no vertex colors); OBJ "
        "smoothing groups ignored; missing normals generated flat; no general OBJ/MTL importer.");
    auto texture = [&](std::string const &path, TextureRole role) -> std::optional<std::size_t> {
      if (path.empty())
        return {};
      checkpoint(stop);
      auto bytes = read(path, 128u * 1024 * 1024, stop);
      auto encoding = role == TextureRole::Diffuse || role == TextureRole::Emissive
                          ? lr::core::ColorEncoding::Srgb
                          : lr::core::ColorEncoding::Linear;
      auto decoded = decodeTga(bytes, encoding, stop);
      if (auto e = std::get_if<RenderError>(&decoded))
        throw std::runtime_error(e->cause == "cancelled" ? "cancelled"
                                                         : e->message + ": " + e->cause + " : " + path);
      auto image = std::move(std::get<lr::core::ImageData>(decoded));
      budget(image.levels[0].size());
      auto index = asset->textures.size();
      asset->textures.push_back({std::make_shared<lr::core::ImageData const>(std::move(image)), {}, role});
      return index;
    };
    for (auto const &p : entry.parts) {
      checkpoint(stop);
      auto bytes = read(p.objPath, 32u * 1024 * 1024, stop);
      std::string_view text(reinterpret_cast<char const *>(bytes.data()), bytes.size());
      auto parsed = parseObj(text, stop);
      if (auto e = std::get_if<RenderError>(&parsed))
        throw std::runtime_error(e->cause == "cancelled" ? "cancelled"
                                                         : e->message + ": " + e->cause + " : " + p.objPath);
      auto mesh = std::move(std::get<lr::core::MeshData>(parsed));
      budget(mesh.positions.size() * sizeof(lr::core::vec3) + mesh.normals.size() * sizeof(lr::core::vec3) +
             mesh.uvs.size() * sizeof(lr::core::vec2) + mesh.indices.size() * 4);
      if (mesh.uvs.empty() && (!p.diffusePath.empty() || !p.normalPath.empty()))
        throw std::runtime_error("textured catalog mesh has no UV");
      MaterialAsset material;
      material.factors.metallic = 0;
      material.factors.roughness = 0.5f;
      material.baseColorTexture = texture(p.diffusePath, TextureRole::Diffuse);
      material.normalTexture = texture(p.normalPath, TextureRole::Normal);
      material.emissiveTexture = texture(p.emissivePath, TextureRole::Emissive);
      if (material.emissiveTexture)
        material.factors.emissive = {1, 1, 1};
      (void)texture(p.specPath, TextureRole::Specular);
      auto bounds = lr::core::boundsOf(mesh);
      if (asset->parts.empty())
        asset->bounds = bounds;
      else {
        for (int axis = 0; axis < 3; ++axis) {
          asset->bounds.min[axis] = std::min(asset->bounds.min[axis], bounds.min[axis]);
          asset->bounds.max[axis] = std::max(asset->bounds.max[axis], bounds.max[axis]);
        }
      }
      auto mi = asset->materials.size();
      asset->materials.push_back(std::move(material));
      asset->parts.push_back({std::make_shared<lr::core::MeshData const>(std::move(mesh)), mi});
    }
    for (auto const &m : asset->materials)
      for (auto i : {m.baseColorTexture, m.normalTexture, m.emissiveTexture})
        if (i && *i >= asset->textures.size())
          throw std::runtime_error("invalid CPU texture index");
    checkpoint(stop);
    return CpuAssetPtr{std::move(asset)};
  } catch (std::exception const &e) {
    return RenderError{ErrorStage::Load, std::string(e.what()) == "cancelled",
                       "TinyRenderer asset decode failed", e.what()};
  }
}
} // namespace demo
