#include "assets/ObjLoader.h"
#include "assets/TgaDecoder.h"
#include <doctest/doctest.h>
using namespace demo;
static std::vector<std::uint8_t> header(unsigned type, unsigned w, unsigned h, unsigned depth,
                                        unsigned desc = 32) {
  std::vector<std::uint8_t> b(18);
  b[2] = type;
  b[12] = w & 255;
  b[13] = w >> 8;
  b[14] = h & 255;
  b[15] = h >> 8;
  b[16] = depth;
  b[17] = desc;
  return b;
}
static std::vector<std::uint8_t> pixels(Result<lr::core::ImageData> const &r) {
  REQUIRE(std::holds_alternative<lr::core::ImageData>(r));
  auto const &i = std::get<lr::core::ImageData>(r);
  REQUIRE(i.levels.size() == 1);
  return i.levels[0];
}
TEST_CASE("tga/rle-bgr-top-rgba-and-truncation") {
  auto b = header(10, 2, 1, 24);
  b.insert(b.end(), {129, 0, 0, 255});
  auto r = decodeTga(b, lr::core::ColorEncoding::Srgb);
  CHECK(pixels(r) == std::vector<std::uint8_t>{255, 0, 0, 255, 255, 0, 0, 255});
  b.pop_back();
  CHECK(std::holds_alternative<RenderError>(decodeTga(b, lr::core::ColorEncoding::Srgb)));
}
TEST_CASE("tga/four-origins-raw-colors-alpha-and-uv-single-flip") {
  const std::vector<std::uint8_t> rgba = {255, 0, 0, 11, 0, 255, 0, 22, 0, 0, 255, 33, 255, 255, 255, 44};
  std::vector<std::uint8_t> actual;
  for (unsigned origin : {0u, 16u, 32u, 48u}) {
    auto b = header(2, 2, 2, 32, origin | 8);
    // Literal BGRA scan orders, independent of decoder coordinate formulas:
    // bottom-left BW/RG; bottom-right WB/GR; top-left RG/BW; top-right GR/WB.
    const std::vector<std::vector<std::uint8_t>> scans = {
        {255, 0, 0, 33, 255, 255, 255, 44, 0, 0, 255, 11, 0, 255, 0, 22},
        {255, 255, 255, 44, 255, 0, 0, 33, 0, 255, 0, 22, 0, 0, 255, 11},
        {0, 0, 255, 11, 0, 255, 0, 22, 255, 0, 0, 33, 255, 255, 255, 44},
        {0, 255, 0, 22, 0, 0, 255, 11, 255, 255, 255, 44, 255, 0, 0, 33}};
    auto const &scan = scans[origin >> 4];
    b.insert(b.end(), scan.begin(), scan.end());
    actual = pixels(decodeTga(b, lr::core::ColorEncoding::Srgb));
    CHECK(actual == rgba);
  }
  auto m = parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nf 1/1 2/2 3/3\n");
  REQUIRE(std::holds_alternative<lr::core::MeshData>(m));
  auto const &uv = std::get<lr::core::MeshData>(m).uvs;
  CHECK(uv[0].y == 0);
  CHECK(uv[2].y == 1);
  CHECK(actual[4 * (unsigned(uv[0].y) * 2 + unsigned(uv[0].x))] == 255);
  CHECK(actual[4 * (unsigned(uv[2].y) * 2 + unsigned(uv[2].x)) + 2] == 255);
}
TEST_CASE("tga/raw24-rawgray-rlegray-and-raw-packet") {
  auto b = header(2, 1, 1, 24);
  b.insert(b.end(), {1, 2, 3});
  CHECK(pixels(decodeTga(b, lr::core::ColorEncoding::Linear)) == std::vector<std::uint8_t>{3, 2, 1, 255});
  b = header(3, 2, 1, 8);
  b.insert(b.end(), {7, 8});
  CHECK(pixels(decodeTga(b, lr::core::ColorEncoding::Linear)) ==
        std::vector<std::uint8_t>{7, 7, 7, 255, 8, 8, 8, 255});
  b[2] = 11;
  b.resize(18);
  b.insert(b.end(), {129, 9});
  CHECK(pixels(decodeTga(b, lr::core::ColorEncoding::Linear)) ==
        std::vector<std::uint8_t>{9, 9, 9, 255, 9, 9, 9, 255});
  b = header(10, 2, 1, 24);
  b.insert(b.end(), {1, 1, 2, 3, 4, 5, 6});
  CHECK(pixels(decodeTga(b, lr::core::ColorEncoding::Linear)) ==
        std::vector<std::uint8_t>{3, 2, 1, 255, 6, 5, 4, 255});
}
TEST_CASE("tga/strict-boundaries-cap-overflow-and-cancellation") {
  for (unsigned type : {2u, 3u, 10u, 11u}) {
    auto b = header(type, 2, 2, type == 3 || type == 11 ? 8 : 24);
    CHECK(std::holds_alternative<RenderError>(decodeTga(b, lr::core::ColorEncoding::Linear)));
  }
  auto b = header(10, 1, 1, 24);
  b.insert(b.end(), {129, 0, 0, 0});
  CHECK(std::holds_alternative<RenderError>(decodeTga(b, lr::core::ColorEncoding::Linear)));
  for (auto b2 : {header(2, 65535, 65535, 32), header(2, 0, 1, 24), header(1, 1, 1, 8), header(3, 1, 1, 16),
                  header(2, 1, 1, 24, 64)})
    CHECK(std::holds_alternative<RenderError>(decodeTga(b2, lr::core::ColorEncoding::Linear)));
  b = header(2, 1, 1, 24);
  b[0] = 255;
  CHECK(std::holds_alternative<RenderError>(decodeTga(b, lr::core::ColorEncoding::Linear)));
  b[0] = 0;
  b[1] = 1;
  CHECK(std::holds_alternative<RenderError>(decodeTga(b, lr::core::ColorEncoding::Linear)));
  b = header(2, 1, 1, 24);
  b.insert(b.end(), {0, 64, 128});
  int n = 0;
  auto r = decodeTga(b, lr::core::ColorEncoding::Linear, [&] { return ++n >= 2; });
  REQUIRE(std::holds_alternative<RenderError>(r));
  CHECK(std::get<RenderError>(r).cause == "cancelled");
}

TEST_CASE("tga/linear-normal-spec-bytes-not-gamma-or-green-flipped") {
  auto b = header(2, 1, 1, 24);
  b.insert(b.end(), {255, 64, 128});
  auto normal = decodeTga(b, lr::core::ColorEncoding::Linear);
  auto color = decodeTga(b, lr::core::ColorEncoding::Srgb);
  CHECK(pixels(normal) == std::vector<std::uint8_t>{128, 64, 255, 255});
  CHECK(pixels(normal) == pixels(color));
  CHECK(std::get<lr::core::ImageData>(normal).encoding == lr::core::ColorEncoding::Linear);
  CHECK(std::get<lr::core::ImageData>(color).encoding == lr::core::ColorEncoding::Srgb);
}

TEST_CASE("tga/rle32-mixed-packets-across-row-with-right-origin") {
  auto b = header(10, 2, 2, 32, 32 | 16 | 8);
  b.insert(b.end(), {130, 3, 2, 1, 17, 0, 6, 5, 4, 66});
  CHECK(pixels(decodeTga(b, lr::core::ColorEncoding::Linear)) ==
        std::vector<std::uint8_t>{1, 2, 3, 17, 1, 2, 3, 17, 4, 5, 6, 66, 1, 2, 3, 17});
  b.pop_back();
  CHECK(std::holds_alternative<RenderError>(decodeTga(b, lr::core::ColorEncoding::Linear)));
}
