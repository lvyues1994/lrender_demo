#pragma once
#include "CpuAsset.h"
namespace demo {
// Fixed read-only TinyRenderer manifest; all paths relative to source root.
// licensePath identifies original provenance: software LICENSE is NOT a model grant.

struct AssetPartEntry {
  std::string objPath, diffusePath, normalPath, emissivePath, specPath;
};
struct AssetEntry {
  ModelId id;
  std::string objPath, licensePath;
  std::vector<AssetPartEntry> parts;
};
class AssetCatalog {
public:
  std::vector<AssetEntry> list() const;
  Result<AssetEntry> describe(ModelId const &) const;
};
} // namespace demo
