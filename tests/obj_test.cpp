#include "assets/ObjLoader.h"
#include <cmath>
#include <doctest/doctest.h>
using namespace demo;
static const std::string triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
TEST_CASE("obj/tuple-seams-negative-indices-and-uv-flipped-once") {
  auto r = parseObj(triangle + "vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 2\nvn 0 0 -3\nf 1/1/1 2/2/1 3/3/1\nf -3/-3/-1 "
                               "-2/-2/-1 -1/-1/-1\nf 1/2/1 2/2/1 3/3/1\n");
  REQUIRE(std::holds_alternative<lr::core::MeshData>(r));
  auto m = std::get<lr::core::MeshData>(r);
  CHECK(m.positions.size() == 7);
  CHECK(m.indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 1, 2});
  CHECK(m.normals.size() == 7);
  CHECK(m.uvs.size() == 7);
  CHECK(m.uvs[6].x == 1);
  CHECK(m.uvs[0].y == 1);
  CHECK(m.uvs[2].y == 0);
  CHECK(m.normals[0].z == 1);
  CHECK(m.normals[3].z == -1);
}
TEST_CASE("obj/reuses-identical-tuples-flat-normal-and-no-uv-policy") {
  auto r = parseObj(triangle + "vn 0 0 1\nf 1//1 2//1 3//1\nf 1//1 2//1 3//1\n");
  REQUIRE(std::holds_alternative<lr::core::MeshData>(r));
  CHECK(std::get<lr::core::MeshData>(r).positions.size() == 3);
  CHECK(std::get<lr::core::MeshData>(r).uvs.empty());
  r = parseObj(triangle + "f 1 2 3\n");
  REQUIRE(std::holds_alternative<lr::core::MeshData>(r));
  auto const &m = std::get<lr::core::MeshData>(r);
  CHECK(m.normals.size() == 3);
  CHECK(m.normals[0].z == 1);
  CHECK(m.indices.size() == 3);
}
TEST_CASE("obj/rejects-malformed-or-unsupported-with-load-errors") {
  for (auto face : {"f 0 2 3\n", "f 1 2 4\n", "f -4 -2 -1\n", "f 1 2\n", "f 1 2 3 1\n", "f 1/ 2/ 3/\n",
                    "f 1//0 2//1 3//1\n", "f 1/a/1 2/1/1 3/1/1\n", "f 1/1 2 3\n", "f 1 1 1\n"}) {
    auto r = parseObj(triangle + "vt 0 0\nvn 0 0 1\n" + face);
    REQUIRE(std::holds_alternative<RenderError>(r));
    CHECK(std::get<RenderError>(r).stage == ErrorStage::Load);
    CHECK_FALSE(std::get<RenderError>(r).cause.empty());
  }
  for (auto input : {"v nan 0 0\nf 1 1 1", "v inf 0 0\nf 1 1 1", "v 1e999 0 0", "vn 0 0 0", "", "v 0 0 0 2"})
    CHECK(std::holds_alternative<RenderError>(parseObj(input)));
}
TEST_CASE("obj/cooperative-stop-and-input-cap") {
  int checks = 0;
  auto r = parseObj(triangle + "f 1 2 3\n", [&] { return ++checks >= 3; });
  REQUIRE(std::holds_alternative<RenderError>(r));
  CHECK(std::get<RenderError>(r).cause == "cancelled");
  CHECK(checks >= 3);
  CHECK(std::holds_alternative<RenderError>(parseObj(std::string(33 * 1024 * 1024, ' '))));
}

TEST_CASE("obj/2d-texture-coordinate-ignores-finite-third-component-like-source-loader") {
  auto r = parseObj(triangle + "vt 0 0 0.494\nvt 1 0 -1.012\nvt 0 1 0.72\nf 1/1 2/2 3/3\n");
  REQUIRE(std::holds_alternative<lr::core::MeshData>(r));
  CHECK(std::get<lr::core::MeshData>(r).uvs[2].y == 0);
  CHECK(std::holds_alternative<RenderError>(parseObj(triangle + "vt 0 0 nan\nf 1/1 2/1 3/1\n")));
}

TEST_CASE("obj/boggie-vertex-rgb-validated-but-unused-like-source-loader") {
  auto r = parseObj("v 0 0 0 0.7 0.7 0.7\nv 1 0 0 0 1 0\nv 0 1 0 1 0 1\nf 1 2 3\n");
  REQUIRE(std::holds_alternative<lr::core::MeshData>(r));
  CHECK(std::get<lr::core::MeshData>(r).positions.size() == 3);
  CHECK(std::holds_alternative<RenderError>(parseObj("v 0 0 0 1 nan 0")));
  CHECK(std::holds_alternative<RenderError>(parseObj("v 0 0 0 2 1 0")));
}
