#pragma once
#include "assets/CpuAsset.h"
#include <lr/scene/World.h>
namespace demo {
// Serial World-owner journal. Caller MUST gate extractDelta from begin until
// commit/rollback, including other scene updates. Only our entities/refs are
// owned; preexisting rig entities survive replacement. Destroy on owner before
// World dies. begin validates/reserves before mutation. step bounds parts, not
// wall-clock GPU upload time. No strong rollback claim for upstream allocator
// failure mid-add: Session fails closed instead of extracting/retrying an
// uncertain delta.
class AssetInstall {
public:
  explicit AssetInstall(lr::scene::World &);
  ~AssetInstall();
  void begin(CpuAssetPtr);
  bool step(std::size_t maxParts);
  void commit();
  void rollback();
  void clear();
  bool active() const;
  std::size_t completedParts() const;
  CpuAssetPtr current() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace demo
