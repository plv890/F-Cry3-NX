#pragma once

#include <cstdint>

namespace rex::x360 {

// Map Xenos component signs through the texture view's guest swizzle. Values
// 4..7 are constants; they must never be biased as sampled texture channels.
constexpr uint32_t MascaraSesgoTextura(uint32_t signos, uint32_t swizzle) {
  uint32_t mascara = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t canal = (swizzle >> (i * 3)) & 7;
    if (canal < 4 && ((signos >> (canal * 2)) & 3) == 2) mascara |= 1u << i;
  }
  return mascara;
}

static_assert(MascaraSesgoTextura(0x0A, 0x0CA) == 0xA);
static_assert(MascaraSesgoTextura(0x00, 0x688) == 0);
static_assert(MascaraSesgoTextura(0xAA, 0xFFF) == 0);

}  // namespace rex::x360
