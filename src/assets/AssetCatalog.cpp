#include "AssetCatalog.h"
namespace demo {
namespace {
AssetPartEntry part(std::string stem, bool glow = false) {
  return {stem + ".obj", stem + "_diffuse.tga", stem + "_nm_tangent.tga", glow ? stem + "_glow.tga" : "",
          stem + "_spec.tga"};
}
AssetEntry entry(std::string id, std::string license, std::vector<AssetPartEntry> p) {
  auto primary = p.front().objPath;
  return {{std::move(id)}, std::move(primary), std::move(license), std::move(p)};
}
} // namespace
std::vector<AssetEntry> AssetCatalog::list() const {
  return {entry("african_head", "obj/african_head/readme.txt",
                {part("obj/african_head/african_head"), part("obj/african_head/african_head_eye_inner"),
                 part("obj/african_head/african_head_eye_outer")}),
          entry("diablo", "obj/diablo3_pose/readme.txt", {part("obj/diablo3_pose/diablo3_pose", true)}),
          entry("boggie", "obj/boggie/readme.txt",
                {part("obj/boggie/body"), part("obj/boggie/head"), part("obj/boggie/eyes")}),
          entry("floor", "LICENSE.txt", {part("obj/floor")})};
}
Result<AssetEntry> AssetCatalog::describe(ModelId const &id) const {
  for (auto const &e : list())
    if (e.id.value == id.value)
      return e;
  return RenderError{ErrorStage::Load, false, "unknown catalog ModelId", id.value};
}
} // namespace demo
