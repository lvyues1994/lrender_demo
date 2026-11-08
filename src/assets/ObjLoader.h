#pragma once
#include "CpuAsset.h"
#include <string_view>
namespace demo {
// Strict 2D triangular subset, not OBJ/MTL import: mixed missing UV rejected,
// absent UV empty, absent normals flat, provided normals unit. XYZ+RGB and third
// vt component validated/ignored like source Model; UV v flipped exactly once.
// Caps:32MiB input,64KiB line,250k source elements/triangles,500k unified vertices.

Result<lr::core::MeshData> parseObj(std::string_view);
Result<lr::core::MeshData> parseObj(std::string_view, std::function<bool()> const &stopRequested);
} // namespace demo
