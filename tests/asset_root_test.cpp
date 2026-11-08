#include "assets/AssetRoot.h"
#include <doctest/doctest.h>
#include <fstream>
namespace fs = std::filesystem;
using namespace demo;
namespace {
struct Fixture {
  fs::path base =
      fs::temp_directory_path() /
      ("lrender-p10-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path root = base / "root";
  Fixture() {
    fs::create_directories(root / "obj");
    for (auto p : {"LICENSE.txt", "obj/floor.obj", "obj/floor_diffuse.tga", "obj/floor_nm_tangent.tga",
                   "obj/floor_spec.tga"})
      fs::copy_file(defaultAssetRoot() / p, root / p);
  }
  ~Fixture() { fs::remove_all(base); }
};
} // namespace
TEST_CASE("asset_root/runtime-root-is-used-not-compiled-fallback") {
  Fixture f;
  CHECK(validateAssetRoot(f.root) == fs::canonical(f.root));
  auto e = std::get<AssetEntry>(AssetCatalog{}.describe({"floor"}));
  CHECK(std::holds_alternative<CpuAssetPtr>(decodeAssetAtRoot(e, f.root, {})));
  std::ofstream(f.root / "obj/floor.obj") << "bad geometry";
  auto broken = decodeAssetAtRoot(e, f.root, {});
  CHECK(std::holds_alternative<RenderError>(broken));
  CHECK_THROWS(validateAssetRoot(f.base / "missing"));
  CHECK_THROWS(validateAssetRoot(f.base));
}
TEST_CASE("asset_root/traversal-absolute-and-symlink-escape-rejected") {
  Fixture f;
  CHECK_THROWS(checkedAssetPath(f.root, "../outside"));
  CHECK_THROWS(checkedAssetPath(f.root, "/etc/passwd"));
  std::ofstream(f.base / "outside") << "private fixture";
  fs::create_symlink(f.base / "outside", f.root / "escape");
  CHECK_THROWS(checkedAssetPath(f.root, "escape"));
  CHECK(checkedAssetPath(f.root, "obj/floor.obj") == fs::canonical(f.root / "obj/floor.obj"));
  auto e = std::get<AssetEntry>(AssetCatalog{}.describe({"floor"}));
  e.parts[0].objPath = "../outside";
  CHECK(std::holds_alternative<RenderError>(decodeAssetAtRoot(e, f.root, {})));
}
