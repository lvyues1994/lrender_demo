#include "ObjLoader.h"
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
namespace demo {
namespace {
void require(bool condition, char const *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void checkpoint(std::function<bool()> const &s) {
  if (s && s())
    throw std::runtime_error("cancelled");
}
float number(std::istringstream &line) {
  double n;
  require(bool(line >> n) && std::isfinite(n) && std::abs(n) <= std::numeric_limits<float>::max(),
          "invalid/nonfinite OBJ number");
  return static_cast<float>(n);
}
lr::core::vec3 unit(double x, double y, double z) {
  auto length = std::hypot(x, y, z);
  require(std::isfinite(length) && length > 0, "degenerate OBJ normal/triangle");
  return {float(x / length), float(y / length), float(z / length)};
}
long long index(std::string const &s, std::size_t count) {
  long long value = 0;
  auto p = std::from_chars(s.data(), s.data() + s.size(), value);
  require(!s.empty() && p.ec == std::errc{} && p.ptr == s.data() + s.size() && value != 0,
          "invalid OBJ index");
  require(value >= -static_cast<long long>(count) && value <= static_cast<long long>(count),
          "OBJ index out of range");
  auto i = value > 0 ? value - 1 : static_cast<long long>(count) + value;
  require(i >= 0 && i < static_cast<long long>(count), "OBJ index out of range");
  return i;
}
} // namespace
Result<lr::core::MeshData> parseObj(std::string_view t) { return parseObj(t, {}); }
Result<lr::core::MeshData> parseObj(std::string_view text, std::function<bool()> const &stop) {
  std::size_t lineNumber = 0;
  try {
    checkpoint(stop);
    require(text.size() <= 32u * 1024 * 1024, "OBJ input exceeds 32MiB limit");
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::vector<lr::core::vec3> positions, normals;
    std::vector<lr::core::vec2> uv;
    lr::core::MeshData mesh;
    std::map<std::array<long long, 3>, std::uint32_t> tuples;
    int uvPolicy = -1;
    std::size_t faces = 0;
    std::string line;
    while (std::getline(input, line)) {
      ++lineNumber;
      checkpoint(stop);
      require(line.size() <= 65536, "OBJ line exceeds 64KiB limit");
      line.resize(line.find('#') == std::string::npos ? line.size() : line.find('#'));
      std::istringstream row(line);
      row.imbue(std::locale::classic());
      std::string kind;
      if (!(row >> kind))
        continue;
      if (kind == "v" || kind == "vn") {
        auto x = number(row), y = number(row), z = number(row);
        std::string extra;
        row >> std::ws;
        if (kind == "v" && !row.eof()) { // Boggie's RGB vertex extension; source Model ignores it.
          for (int i = 0; i < 3; ++i) {
            auto color = number(row);
            require(color >= 0 && color <= 1, "invalid OBJ vertex RGB");
          }
        }
        require(!(row >> extra), "unsupported OBJ vertex/normal fields");
        auto &values = kind == "v" ? positions : normals;
        require(values.size() < 250000, "OBJ source element limit");
        values.push_back(kind == "v" ? lr::core::vec3{x, y, z} : unit(x, y, z));
      } else if (kind == "vt") {
        auto u = number(row), v = number(row);
        std::string third;
        if (row >> third) {
          std::istringstream w(third);
          w.imbue(std::locale::classic());
          (void)number(w); // Validate/discard optional third texture component for 2D, as source Model does.
          std::string extra;
          require(!(w >> extra) && !(row >> extra), "unsupported OBJ texture fields");
        }
        require(uv.size() < 250000, "OBJ source element limit");
        uv.push_back({u, 1.0f - v});
        require(std::isfinite(uv.back().y), "OBJ flipped UV overflow");
      } else if (kind == "f") {
        require(++faces <= 250000, "OBJ triangle limit");
        std::array<std::array<long long, 3>, 3> keys;
        std::string token;
        for (auto &key : keys) {
          require(bool(row >> token), "truncated OBJ triangle");
          key = {-1, -1, -1};
          auto first = token.find('/');
          auto second = first == std::string::npos ? first : token.find('/', first + 1);
          key[0] = index(token.substr(0, first), positions.size());
          if (first != std::string::npos) {
            if (second == std::string::npos) {
              key[1] = index(token.substr(first + 1), uv.size());
            } else {
              require(token.find('/', second + 1) == std::string::npos, "unsupported OBJ index tuple");
              if (second > first + 1)
                key[1] = index(token.substr(first + 1, second - first - 1), uv.size());
              key[2] = index(token.substr(second + 1), normals.size());
            }
          }
          int hasUv = key[1] >= 0;
          if (uvPolicy < 0)
            uvPolicy = hasUv;
          require(uvPolicy == hasUv, "mixed missing OBJ UV is unsupported");
        }
        require(!(row >> token), "unsupported non-triangle OBJ face");
        auto a = positions[keys[0][0]], b = positions[keys[1][0]], c = positions[keys[2][0]];
        double ux = double(b.x) - a.x, uy = double(b.y) - a.y, uz = double(b.z) - a.z, vx = double(c.x) - a.x,
               vy = double(c.y) - a.y, vz = double(c.z) - a.z;
        auto flat = unit(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);
        for (auto key : keys) {
          auto n = key[2] >= 0 ? normals[key[2]] : flat;
          if (key[2] < 0)
            key[2] = -2 - static_cast<long long>(faces);
          auto found = tuples.find(key);
          if (found == tuples.end()) {
            require(mesh.positions.size() < 500000, "OBJ unified vertex limit");
            auto id = static_cast<std::uint32_t>(mesh.positions.size());
            tuples.emplace(key, id);
            mesh.positions.push_back(positions[key[0]]);
            mesh.normals.push_back(n);
            if (uvPolicy)
              mesh.uvs.push_back(uv[key[1]]);
            mesh.indices.push_back(id);
          } else
            mesh.indices.push_back(found->second);
        }
      } else
        require(kind == "o" || kind == "g" || kind == "s",
                "unsupported OBJ directive (no MTL/curves importer)");
    }
    require(!mesh.indices.empty(), "OBJ has no triangles");
    checkpoint(stop);
    return mesh;
  } catch (std::exception const &e) {
    return RenderError{ErrorStage::Load, std::string(e.what()) == "cancelled",
                       "OBJ line " + std::to_string(lineNumber), e.what()};
  }
}
} // namespace demo
