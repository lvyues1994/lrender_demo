#include "assets/TinyrendererAdapter.h"
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <iostream>
using namespace demo;
TEST_CASE("asset_mapping/catalog-modelid-not-path-and-stop") {
  AssetCatalog c;
  CHECK(c.list().size() == 4);
  CHECK(std::holds_alternative<RenderError>(c.describe({"../floor.obj"})));
  auto r = c.describe({"african_head"});
  REQUIRE(std::holds_alternative<AssetEntry>(r));
  auto entry = std::get<AssetEntry>(r);
  CHECK(entry.parts.size() == 3);
  CHECK(std::holds_alternative<RenderError>(decodeAsset(entry, [] { return true; })));
  entry.parts[0].objPath = "../LICENSE.txt";
  CHECK(std::holds_alternative<RenderError>(decodeAsset(entry, {})));
}
TEST_CASE("asset_mapping/real-multipart-counts-bounds-and-color-material-semantics") {
  struct Expect {
    const char *id;
    unsigned parts, faces, textures;
  };
  for (auto e : {Expect{"african_head", 3, 4012, 9}, Expect{"diablo", 1, 5022, 4},
                 Expect{"boggie", 3, 6966, 9}, Expect{"floor", 1, 2, 3}}) {
    auto description = AssetCatalog{}.describe({e.id});
    REQUIRE(std::holds_alternative<AssetEntry>(description));
    auto result = decodeAsset(std::get<AssetEntry>(description), {});
    if (auto error = std::get_if<RenderError>(&result))
      std::cerr << e.id << " " << error->message << " : " << error->cause << "\n";
    REQUIRE(std::holds_alternative<CpuAssetPtr>(result));
    auto a = std::get<CpuAssetPtr>(result);
    REQUIRE(a);
    CHECK(a->parts.size() == e.parts);
    CHECK(a->materials.size() == e.parts);
    CHECK(a->textures.size() == e.textures);
    CHECK_FALSE(a->notices.empty());
    CHECK_FALSE(a->warnings.empty());
    std::vector<unsigned> dimensions;
    if (e.id == std::string("african_head"))
      dimensions = {1024, 1024, 1024, 256, 256, 256, 64, 256, 64};
    if (e.id == std::string("diablo"))
      dimensions = {1024, 1024, 1024, 1024};
    if (e.id == std::string("boggie"))
      dimensions = {2048, 2048, 2048, 1024, 1024, 1024, 256, 256, 256};
    if (e.id == std::string("floor"))
      dimensions = {600, 512, 1};
    REQUIRE(dimensions.size() == a->textures.size());
    for (std::size_t i = 0; i < dimensions.size(); ++i) {
      CHECK(a->textures[i].image->extent.x == dimensions[i]);
      CHECK(a->textures[i].image->extent.y == dimensions[i]);
    }
    for (auto const &part : std::get<AssetEntry>(description).parts)
      CHECK(part.normalPath.find("_nm_tangent.tga") != std::string::npos);
    for (int axis = 0; axis < 3; ++axis) {
      CHECK(std::isfinite(a->bounds.min[axis]));
      CHECK(std::isfinite(a->bounds.max[axis]));
      CHECK(a->bounds.max[axis] >= a->bounds.min[axis]);
    }
    unsigned faces = 0, vertices = 0;
    for (auto const &p : a->parts) {
      REQUIRE(p.mesh);
      auto const &m = *p.mesh;
      faces += m.indices.size() / 3;
      vertices += m.positions.size();
      CHECK(m.normals.size() == m.positions.size());
      CHECK(m.uvs.size() == m.positions.size());
      CHECK(std::all_of(m.indices.begin(), m.indices.end(), [&](auto i) { return i < m.positions.size(); }));
    }
    CHECK(faces == e.faces);
    float bx = 1, bz = 1;
    unsigned expectedVertices = 4;
    if (e.id == std::string("african_head")) {
      bx = .66248f;
      bz = .675105f;
      expectedVertices = 2143;
    }
    if (e.id == std::string("diablo")) {
      bx = .756634f;
      bz = .809458f;
      expectedVertices = 3263;
    }
    if (e.id == std::string("boggie")) {
      bx = .474565f;
      bz = .234883f;
      expectedVertices = 4032;
    }
    CHECK(vertices == expectedVertices);
    CHECK(a->bounds.min.x == doctest::Approx(-bx));
    CHECK(a->bounds.max.x == doctest::Approx(bx));
    CHECK(a->bounds.min.z == doctest::Approx(-bz));
    CHECK(a->bounds.max.z == doctest::Approx(bz));
    CHECK(a->bounds.min.y == doctest::Approx(-1));
    CHECK(a->bounds.max.y == doctest::Approx(e.id == std::string("floor") ? -1 : 1));
    CHECK(std::isfinite(a->bounds.min.x));
    CHECK(std::isfinite(a->bounds.max.z));
    CHECK(a->bounds.max.x > a->bounds.min.x);
    for (auto const &t : a->textures) {
      REQUIRE(t.image);
      CHECK(t.image->extent.x > 0);
      CHECK(t.image->extent.y > 0);
      CHECK(t.image->levels[0].size() == std::size_t(t.image->extent.x) * t.image->extent.y * 4);
      CHECK(t.image->encoding == ((t.role == TextureRole::Diffuse || t.role == TextureRole::Emissive)
                                      ? lr::core::ColorEncoding::Srgb
                                      : lr::core::ColorEncoding::Linear));
    }
    for (auto const &m : a->materials) {
      CHECK(m.factors.metallic == 0);
      CHECK(m.factors.roughness == 0.5f);
      CHECK_FALSE(m.factors.metallicRoughnessTexture);
      CHECK_FALSE(m.factors.normalTexture);
      REQUIRE(m.baseColorTexture);
      REQUIRE(m.normalTexture);
      CHECK(*m.normalTexture < a->textures.size());
      CHECK(a->textures[*m.normalTexture].role == TextureRole::Normal);
      if (e.id == std::string("diablo")) {
        REQUIRE(m.emissiveTexture);
        CHECK(a->textures[*m.emissiveTexture].role == TextureRole::Emissive);
        CHECK(m.factors.emissive.x == 1);
      }
    }
    std::cout << "Asset " << e.id << " parts=" << a->parts.size() << " vertices=" << vertices
              << " faces=" << faces << " textures=" << a->textures.size() << " bounds=[" << a->bounds.min.x
              << "," << a->bounds.min.y << "," << a->bounds.min.z << "]..[" << a->bounds.max.x << ","
              << a->bounds.max.y << "," << a->bounds.max.z << "]\n";
  }
}

TEST_CASE("asset_mapping/mid-parse-and-mid-image-cancellation-is-not-failure") {
  auto description = AssetCatalog{}.describe({"african_head"});
  REQUIRE(std::holds_alternative<AssetEntry>(description));
  for (int threshold : {12, 10000}) {
    int checks = 0;
    auto result = decodeAsset(std::get<AssetEntry>(description), [&] { return ++checks >= threshold; });
    REQUIRE(std::holds_alternative<RenderError>(result));
    CHECK(std::get<RenderError>(result).cause == "cancelled");
    CHECK(std::get<RenderError>(result).recoverable);
  }
}

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
TEST_CASE("asset_mapping/p10-readable-catalog-matches-fixed-runtime-manifest") {
  QFile file(QString::fromUtf8(DEMO_METADATA_ROOT) + "/assets/catalog.json");
  REQUIRE(file.open(QIODevice::ReadOnly));
  auto doc = QJsonDocument::fromJson(file.readAll());
  REQUIRE(doc.isObject());
  auto object = doc.object();
  CHECK(object["sourceSha"].toString() == "97eb7a480e762285899a93c5d21e70a79e60782e");
  auto models = object["models"].toArray();
  auto actual = demo::AssetCatalog{}.list();
  REQUIRE(models.size() == int(actual.size()));
  for (int i = 0; i < models.size(); ++i) {
    auto m = models[i].toObject();
    CHECK(m["id"].toString().toStdString() == actual[i].id.value);
    CHECK(m["noticePath"].toString().toStdString() == actual[i].licensePath);
    auto parts = m["parts"].toArray();
    REQUIRE(parts.size() == int(actual[i].parts.size()));
    for (int j = 0; j < parts.size(); ++j) {
      auto p = parts[j].toObject();
      auto a = actual[i].parts[j];
      CHECK(p["obj"].toString().toStdString() == a.objPath);
      CHECK(p["diffuse"].toString().toStdString() == a.diffusePath);
      CHECK(p["normal"].toString().toStdString() == a.normalPath);
      CHECK(p["specularUnbound"].toString().toStdString() == a.specPath);
      CHECK(p["emissive"].toString().toStdString() == a.emissivePath);
    }
  }
  QFile notices(QString::fromUtf8(DEMO_METADATA_ROOT) + "/licenses/ASSET_NOTICES.txt");
  REQUIRE(notices.open(QIODevice::ReadOnly));
  auto text = notices.readAll();
  CHECK(text.contains("Vidar Rapp"));
  CHECK(text.contains("Samuel"));
  CHECK(text.contains("Bojana"));
  CHECK(text.contains("educational"));
  CHECK(text.contains("not a blanket"));
  CHECK(text.contains("no separate model grant"));
}
