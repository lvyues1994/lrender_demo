#pragma once
#include "TinyrendererAdapter.h"
#include <filesystem>
namespace demo {
std::filesystem::path defaultAssetRoot();
std::filesystem::path validateAssetRoot(std::filesystem::path const &);
std::filesystem::path checkedAssetPath(std::filesystem::path const &, std::string const &);
Result<CpuAssetPtr> decodeAssetAtRoot(AssetEntry const &, std::filesystem::path const &,
                                      std::function<bool()>);
} // namespace demo
