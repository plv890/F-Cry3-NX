// Universal Xbox 360 shader descriptor: what a game's shader library hands the renderer for one translated shader.
//
// The game identifies the Xbox 360 microcode of each draw and looks up the matching entry. The renderer reads only
// the universal fields here (the SPIR-V to compile, the vertex elements, the samplers, the output and discard
// masks); it never touches the game's own storage. `juego` is an opaque escape hatch for the game's library (its
// container pointer, a map key); the renderer never dereferences it. `huella` is the fingerprint of the masked
// microcode (identification); `huella_contenedor` is the fingerprint of the original container, which a game may
// use to recognise specific shaders (its sky, glow or composition passes).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace rex::x360 {

struct ElementoVertice {
  uint16_t instruccion = 0;  // index of the fetch instruction in the microcode
  uint8_t uso = 0;           // DeclUsage: 0 position, 3 normal, 5 texcoord, 10 color...
  uint8_t indice_uso = 0;
  // The fetch reads at an index the shader computes (its source is not r0.x, the vertex's own index): Dead Space
  // 2's particles read one record per particle at vertex / 4. The translated shader reads it from a storage buffer
  // itself (dsVfetch, descriptor `k` = this element's position in `elementos`); the renderer binds the whole buffer
  // and describes the fetch instead of making it a vertex attribute.
  bool manual = false;
  bool redondeado = false;  // is_index_rounded: the index is rounded, not truncated
};

struct SamplerShader {
  uint16_t registro = 0;
  uint16_t tipo = 0;  // D3DXPARAMETER_TYPE: 10-12 = 2D, 13 = 3D, 14 = cube
  bool mapa_sombras = false;
};

struct EntradaShader {
  std::span<const uint32_t> spirv;  // the translated SPIR-V, owned by the game's library
  const void* juego = nullptr;      // opaque: the game's own container/key; the renderer never reads it
  bool vertices = false;
  uint32_t numero = 0;              // position in the library, for reports
  std::vector<uint32_t> microcodigo;  // in host byte order, with the fetches masked
  uint64_t huella = 0;              // XXH3 of that masked microcode (identification)
  uint64_t huella_contenedor = 0;   // XXH3 of the original container (a game recognises its own passes)
  std::vector<ElementoVertice> elementos;
  std::vector<SamplerShader> samplers;
  uint32_t salidas = 0;  // pixel shader: bits COLOR0..3 and DEPTH (PixelShaderOutputs)
  uint32_t kills = 0;
  bool descarta = false;
  bool sombra_minimo = false;
  uint32_t constantes_bytes = 256 * 16;
};

}  // namespace rex::x360
