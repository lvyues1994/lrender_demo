#include "AssetRoot.h"
#include <stdexcept>
namespace demo {
namespace fs = std::filesystem;
fs::path defaultAssetRoot() { return DEMO_TINYRENDERER_ROOT; }
fs::path checkedAssetPath(fs::path const &input, std::string const &relative) {
  fs::path p(relative);
  if (p.empty() || p.is_absolute())
    throw std::runtime_error("asset path must be relative");
  for (auto const &x : p)
    if (x == "..")
      throw std::runtime_error("asset path escape rejected");
  auto root = fs::canonical(input), resolved = fs::canonical(root / p);
  auto a = root.begin(), b = resolved.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == resolved.end() || *a != *b)
      throw std::runtime_error("asset symlink escape rejected");
  if (!fs::is_regular_file(resolved))
    throw std::runtime_error("asset is not a regular file");
  return resolved;
}
fs::path validateAssetRoot(fs::path const &input) {
  if (!fs::is_directory(input))
    throw std::runtime_error("asset root is not an existing directory: " + input.string());
  auto root = fs::canonical(input);
  (void)checkedAssetPath(root, "LICENSE.txt");
  if (!fs::is_directory(root / "obj"))
    throw std::runtime_error("asset root lacks obj directory: " + root.string());
  return root;
}
} // namespace demo
