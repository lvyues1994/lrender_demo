#pragma once
#include "render/SessionTypes.h"
namespace demo {
// Immutable output handles; material optionals are CPU vector indices, never engine IDs.
// Specular is retained Linear provenance but deliberately unbound, not metallic.

enum class TextureRole { Diffuse, Normal, Emissive, Specular };
struct TextureAsset {
  lr::core::ImageHandle image;
  lr::core::TextureSampler sampler;
  TextureRole role = TextureRole::Diffuse;
};
struct MaterialAsset {
  lr::core::MaterialDesc factors;
  std::optional<std::size_t> baseColorTexture, normalTexture, emissiveTexture;
};
struct PartAsset {
  lr::core::MeshHandle mesh;
  std::size_t material = 0;
};
struct CpuAsset {
  ModelId model;
  std::vector<TextureAsset> textures;
  std::vector<MaterialAsset> materials;
  std::vector<PartAsset> parts;
  lr::core::Aabb bounds;
  std::vector<std::string> warnings, notices;
};
using CpuAssetPtr = std::shared_ptr<CpuAsset const>;
} // namespace demo
