#include "presentation/NativeSurfaceFactory.h"
#include <doctest/doctest.h>
using namespace demo;
TEST_CASE("presentation/shared-physical-prefers-live-host-over-cached-wsi") {
  CHECK(resolveSharedPhysicalExtent({220, 160}, {160, 120}) == lr::core::ivec2{220, 160});
  CHECK(resolveSharedPhysicalExtent({200, 150}, {200, 150}) == lr::core::ivec2{200, 150});
  CHECK(resolveSharedPhysicalExtent({0, 0}, {160, 120}) == lr::core::ivec2{160, 120});
  CHECK(resolveSharedPhysicalExtent({180, 90}, {0, 0}) == lr::core::ivec2{180, 90});
  CHECK(resolveSharedPhysicalExtent({0, 0}, {0, 0}) == lr::core::ivec2{0, 0});
}
