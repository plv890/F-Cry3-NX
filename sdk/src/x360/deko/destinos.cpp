// nfsmw - deko3d renderer: frames and work slots, render targets, copies (resolve and clear), the output pass with
// the game's gamma ramp, the DKSH cache file and the statistics. See nfsmw_dk_render.h.

#include <rex/x360/deko/destinos.h>

#include <functional>  // fc3-switch: rectangle list geometry shader
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <rex/memory/utils.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>

#include <rex/ui/deko3d/compilador.h>
#include <rex/ui/deko3d/presentador.h>
#include <rex/x360/deko/render.h>
#include <rex/x360/xenos.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif

// fc3-switch 2026-11-07 (GPU experiments, default off = previous behaviour; A/B by TOML):
//  - fc3_dk_copias_sin_barrera (N2): back-to-back 3D copies (Copiar3D) with no draw between them get no Fragments
//    barrier between each other when the next copy neither reads memory an earlier copy since the last barrier
//    wrote, nor writes memory one of them read. The barrier before the first draw/clear after the copies stays
//    (AntesDe3D, copia3d_pendiente_). Hazards are checked on the image memory ranges (block, offset, bytes).
//  - fc3_dk_copiar_alto_usado (N13): a whole-target restore (RestaurarContenido) and the 8888 color -> depth alias
//    copy (dibujos.cpp) copy only the rows the game uses (alto_usado, when known) instead of the whole
//    max(720, pitch)-tall target (1280x1280 for the 1280x704 scene). RISK: rows below alto_usado keep their old
//    content; if the game later draws or samples below them (alto_usado grows), those rows are stale.
// fc3-switch 2026-11-09 HACK (black arm): resolved textures of this many KiB or less are never evicted by the budget
// (0 = off). FC3 resolves Jason's tattoo mask (256x512 at 0F055000) once and samples it later; evicted, it was read
// from guest memory, where resolves are not written back: zeros, a black arm. All of them together are capped at 64 MiB.
REXCVAR_DEFINE_BOOL(fc3_sonda, false, "fc3 enhancements",
                    "DIAGNOSTIC: the dump frame keeps only three 8x8 probes (inside/left/above the house block) of the "
                    "target after every draw and resolve, written as one file (sonda_NN.bin/.txt)");
REXCVAR_DEFINE_INT32(fc3_dk_resueltas_fijas_kb, 0, "fc3 enhancements",
                     "Resolved textures this small (KiB) are kept over the budget (0 = off)");
REXCVAR_DEFINE_BOOL(fc3_dk_copias_sin_barrera, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: no Fragments barrier between back-to-back independent 3D copies");
// fc3-switch 2026-11-11 EXPERIMENT (flickering AO dots): the 8888 color -> depth/stencil alias import writes depth
// with gl_FragDepth and stencil bit by bit behind the GPU's ZCULL data. 1: the next 3D work invalidates ZCULL;
// 2: a Full barrier with the invalidation right after the import. 0 = old behaviour.
// fc3-switch 2026-11-11 FIX CANDIDATE (AO specks): on the Xbox 360 an 8888 color target and a depth target at the same
// EDRAM base are the same bytes, so a depth clear also clears the color view (R = stencil, G/B/A = depth bytes). FC3
// clears z 750, then writes only R (stencil) of color 750 with alpha-killed pixels, then tests that stencil (via the
// color->z alias import) in the AO pass. Without this the killed pixels kept stale color bytes and the AO's stencil mask
// flickered. true: a depth clear also clears an 8888 color target already seen at that base and pitch.
// fc3-switch 2026-11-11 DIAGNOSTIC (AO specks vanish in frames that stall): the CPU waits for the GPU to go idle at
// every present, so no frame's GPU work overlaps the next frame's recording. Costs fps.
REXCVAR_DEFINE_BOOL(fc3_dk_esperar_gpu_fotograma, false, "fc3 enhancements",
                    "deko3d DIAGNOSTIC: wait for the GPU to be idle at every present");
REXCVAR_DEFINE_BOOL(fc3_dk_alias_borrado, false, "fc3 enhancements",
                    "deko3d FIX CANDIDATE: a depth clear also clears the aliased 8888 color target (EDRAM aliasing)");
REXCVAR_DEFINE_INT32(fc3_dk_alias_zcull, 0, "fc3 enhancements",
                     "deko3d EXPERIMENT: ZCULL invalidation after the color->depth alias import (0 off, 1 lazy, 2 full barrier)");
REXCVAR_DEFINE_BOOL(fc3_dk_copiar_alto_usado, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: target restores and color->depth alias copies copy only the used rows");

// Settings of the renderer that the game hooks also read (nfsmw_nativo_ganchos.cpp). The Vulkan build defines them
// in nfsmw_nativo_dibujos.cpp, which this build leaves out: same names, types and defaults.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_ps_solo_alfa, true, "NFSMW",
                    "Renderizador nativo: en las pasadas sin destino de color, el pixel shader sin las escrituras "
                    "de color (queda la prueba de alfa)");
REXCVAR_DEFINE_INT32(nfsmw_nativo_ps_solo_alfa_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba): con N > 0 alterna cada N segundos el pixel shader con y sin las "
                     "escrituras de color en las pasadas sin color");
REXCVAR_DEFINE_BOOL(nfsmw_sombras_sin_vegetacion, true, "NFSMW",
                    "No dibujar en el mapa de sombras lo que lleva prueba de alfa: arboles, arbustos y vallas de "
                    "alambre. Los arboles dejan de proyectar sombra");
// The same as the Vulkan build's (nfsmw_nativo_destinos.cpp), read once.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_rampa_gamma, true, "NFSMW",
                    "Renderizador nativo: aplica en la salida la rampa de gamma que carga el juego, como la pantalla "
                    "de la Xbox 360 (sin ella, los medios tonos y las sombras salen mas oscuros). false: la imagen tal "
                    "cual, como antes de la build 137")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The same as the Vulkan build's (nfsmw_nativo_destinos.cpp).
REXCVAR_DEFINE_INT32(nfsmw_nativo_leer_resueltas_texels, 4096, "NFSMW",
                     "Renderizador nativo: las texturas resueltas de hasta estos texels se copian "
                     "tambien a la memoria del invitado, que el juego lee para su exposicion (0 = "
                     "ninguna; 4096 = 64x64, las que usa la exposicion; 57600 = 320x180)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_borrados_repetidos, true, "NFSMW",
                    "Renderizador nativo (20/09 noche): saltarse un borrado cuando el destino ya esta borrado "
                    "con ese mismo valor y no se ha dibujado nada desde entonces. No cambia ni un pixel; "
                    "'borrados saltados' en el informe C2 dice cuantos se ahorran");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_sin_copia, true, "NFSMW",
                    "Renderizador nativo (17/09, build 154; por defecto desde la 161, medido en consola: -2,36 ms "
                    "de los 7,41 de copias): al resolver un destino entero a una textura del mismo "
                    "tamano y formato, intercambia las imagenes en vez de copiar los pixeles");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_sin_borrado, true, "NFSMW",
                    "Renderizador nativo (20/09): intercambiar la imagen tambien cuando la orden de resolve no "
                    "borra el destino. Se lleva por delante la copia de 1600x1600 del segundo mapa de sombras "
                    "(2,4 ms reales). Si el juego dibuja encima sin borrar, se restaura");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sombra_minimo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el mapa de sombras del mundo ya no se restaura copiando "
                    "1600x1600; los coches se dibujan sobre el destino borrado y el mundo muestrea el minimo de las dos "
                    "texturas. Misma imagen. Necesita la biblioteca con tfetch2DSombraMin. Se comprueba sola. false = "
                    "copiar como antes");
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);  // nfsmw_dk_dibujos.cpp
REXCVAR_DEFINE_BOOL(nfsmw_nativo_frontal_perezoso, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el Swap pinta el frontal desde su destino de render en vez "
                    "de copiarlo antes a la textura. Misma imagen; con FXAA se copia como siempre. false = copiar "
                    "siempre");
// deko3d only.
REXCVAR_DEFINE_BOOL(nfsmw_dk_borrar_area_util, true, "NFSMW",
                    "deko3d: los borrados solo cubren las filas que el juego usa del destino (la escena de 1280x720 "
                    "vive en un destino de 1280x1280); si algo llega mas abajo, esa banda se borra antes. Misma imagen. "
                    "false = borrar el destino entero");
REXCVAR_DEFINE_BOOL(nfsmw_dk_borrado_color_aplazado, true, "NFSMW",
                    "deko3d: el borrado de un destino de color se hace al volver a usarlo, como el de profundidad (y "
                    "deja pintar el frontal desde el destino). false = se borra en el acto");
REXCVAR_DEFINE_BOOL(nfsmw_dk_borrado_z_aplazado, true, "NFSMW",
                    "deko3d: el borrado de una profundidad se hace al volver a usarla para dibujar, justo despues de "
                    "enlazarla, para que el ZCULL (que deko3d tira al cambiar de profundidad) empiece el pase valido. "
                    "false = se borra en el acto");
REXCVAR_DEFINE_BOOL(nfsmw_dk_compresion_profundidad, true, "NFSMW",
                    "deko3d: los destinos de profundidad (y sus texturas resueltas) con compresion por hardware: menos "
                    "ancho de banda en la escena y borrados casi gratis. Sus copias van por el motor 3D. false = sin "
                    "comprimir, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_dk_compresion_color, true, "NFSMW",
                    "deko3d: los destinos de color con compresion por hardware (DkImageFlags_HwCompression), como "
                    "SwitchWakerHD. false = sin comprimir")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_dk_subida_mb, 32, "NFSMW",
                     "deko3d: MB del anillo de subida por ranura de trabajo (vertices, indices, constantes y "
                     "texturas). Si un fotograma no cabe se envia y sigue en la siguiente ranura")
    .range(8, 128)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_dk_resueltas_mb, 192, "NFSMW",
                     "deko3d (fc3-switch): MB de texturas resueltas antes de expulsar las menos usadas. 0 = sin limite")
    .range(0, 2048);
REXCVAR_DEFINE_BOOL(fc3_hdr_dynamic_addresses, true, "FC3",
                    "Apply the guarded HDR reinterpret resolve by format/shape/command history at any destination address");
REXCVAR_DEFINE_BOOL(fc3_trace_hdr_chain, false, "FC3",
                    "Capture raw HDR resolves, post-process source and actual output from one frame");
// fc3-switch 2026-11-07 (DIAGNOSTIC): raw .bin + manifest for every texture of a normal frame dump, not only in the
// fc3_trace_hdr_chain mode (exact values; the BMPs are 8-bit previews).
REXCVAR_DEFINE_STRING(fc3_volcado_copias_en, "", "fc3 enhancements",
                      "DIAGNOSTIC: hex guest address; in a dump frame every resolve into it is dumped when written");
// fc3-switch 2026-11-09 HACK (shoulder staircase / glowing patch): the CPU culls lights per 16x16 tile with the 80x44
// min/max depth it reads back; read 1-3 frames late, tiles at the edges of moving characters lost their light (16-px
// blocks; SR1 with every read-back synchronous fixed it at 20 -> 15 fps). This makes only that one synchronous.
// fc3-switch 2026-11-09 HACK: read-backs whose GPU work finished are written to guest memory at every resolve.
// fc3-switch 2026-11-09 HACK: the 80x44 min/max depth read-back is dilated by N tiles before the CPU sees it.
REXCVAR_DEFINE_INT32(fc3_dk_minmax_dilatar, 0, "fc3 enhancements",
                     "Tiles of dilation of the 80x44 min/max depth read-back (light-tile culling; 0 = off)");
REXCVAR_DEFINE_BOOL(fc3_dk_lecturas_ansiosas, false, "fc3 enhancements",
                    "Finished resolve read-backs reach guest memory at every resolve, not only at the present");
REXCVAR_DEFINE_BOOL(fc3_dk_lecturas_sincronas_minmax, false, "fc3 enhancements",
                    "Only the 80x44 min/max depth read-back is synchronous (light-tile culling on current depth)");
REXCVAR_DEFINE_BOOL(fc3_dk_lecturas_sincronas, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: resolve read-backs reach guest memory before the resolve returns (GPU stall)");
REXCVAR_DEFINE_BOOL(fc3_volcado_raw, false, "FC3", "Frame dumps also write raw .bin files of every resolved texture");
REXCVAR_DEFINE_BOOL(fc3_debug_shader_overrides, false, "FC3",
                    "Load debug-shaders/color_to_depth.frag beside the NRO at startup; invalid overrides fall back to embedded GLSL")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_color_depth_swap40, true, "FC3",
                    "Apply Xenos 40-sample column swap when reinterpreting 32-bit color EDRAM as depth/stencil");
REXCVAR_DEFINE_BOOL(fc3_trace_lighting_checkpoint, false, "FC3",
                    "Capture only the stencil import, pre-light depth and final HDR in requested frame dumps");
REXCVAR_DEFINE_INT32(fc3_volcado_omitir, 0, "NFSMW",
                     "deko3d (fc3-switch, DIAGNOSTIC): the frame dump skips its first N resolves (to reach the later ones "
                     "within its 48 MB)")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(nfsmw_dk_perfil_gpu_s, 20, "NFSMW",
                     "deko3d: cada cuantos segundos se mide un fotograma dibujo a dibujo en la GPU (una marca de tiempo "
                     "antes y despues de cada dibujo; ese fotograma va algo mas lento) para el informe de pases y "
                     "shaders mas caros. 0 = nunca");
REXCVAR_DEFINE_BOOL(nfsmw_dk_perfil_barrera, true, "NFSMW",
                    "deko3d: en el fotograma medido, el primer dibujo de cada pase se mide despues de que la GPU acabe "
                    "todo lo anterior (una barrera completa), para no contarle trabajo ajeno (copias, borrados, esperas)");
REXCVAR_DEFINE_BOOL(nfsmw_dk_perfil_variantes, true, "NFSMW",
                    "deko3d: en el fotograma medido, los dibujos a pantalla completa con texturas que se pueden repetir "
                    "se dibujan ademas con el sampler 0, con texturas vacias de 1x1 y escribiendo solo el destino 0, "
                    "antes de su copia real: que parte de su coste es el muestreo, la lectura o la salida");
REXCVAR_DEFINE_INT32(nfsmw_dk_geometria_mb, 64, "NFSMW",
                     "deko3d: MB para los vertices e indices que se repiten entre fotogramas (nfsmw_dk_geometria.h): "
                     "un rango visto dos veces con el mismo contenido (XXH3) se queda y los dibujos siguientes lo usan "
                     "sin copiarlo ni convertirlo. 0 = apagado, todo al anillo como antes")
    .range(0, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_dk_geometria_muestreo, true, "NFSMW",
                    "deko3d: los rangos guardados se recomprueban leyendo 1 de cada 8 lineas de 64 bytes (rotando, "
                    "asi cada linea se lee al menos cada 8 fotogramas) mas la primera y la ultima, en vez de todo. Las "
                    "20000 primeras comprobaciones hacen tambien la huella completa: con un solo desacuerdo se apaga "
                    "solo. false = huella completa cada fotograma")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void RexSwitchPerfCount(unsigned id);
// switch_perf.cpp: a frame over 45 ms, in CPU counter ticks, for the stack sampler's "durante los tirones" section.
extern "C" void RexSwitchPerfTiron(uint64_t inicio, uint64_t fin);

namespace {
// Long frames since the last 10 s report (ring thread only, like the report). The cumulative histogram
// cubetas_swap_ was never printed: a 10 s average of 28-30 fps hid whether the frames were a steady 33/50 ms
// alternation or a few freezes, which is what a hitch report needs.
uint32_t g_tirones_45 = 0;    // swap to swap over 45 ms (a missed flip at swap interval 2)
uint32_t g_tirones_100 = 0;   // over 100 ms (a visible freeze)
double g_tirones_ms = 0.0;    // time spent in those over 45 ms
double g_peor_intervalo_ms = 0.0;
}  // namespace

// The 30 FPS guard (nfsmw_guardia_30) lives in nfsmw_recorte_sombras.cpp.
namespace rex::x360 {

// Guest waits handled by the ring thread (nfsmw_nativo_peticion.h). The Vulkan build defines it in
// nfsmw_nativo_dibujos.cpp; this build has no upload deduplication yet.
std::atomic<uint32_t> g_sincronizaciones_anillo{0};

namespace {

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using deko::ContextoSalidaDeko3d;

constexpr uint32_t kAltoMaximoDestino = 2048;
constexpr uint32_t kRanuras = 3;
// Read-backs per work slot: a frame has two of 64x64 (32 KB); room for 16 of them.
constexpr uint32_t kBytesLecturaRanura = 256u << 10;
// The shadow map (nfsmw_nativo_sombra_minimo): its side, and the clean cycles watched before applying (~10 s of race).
constexpr uint32_t kLadoSombras = 1600;
constexpr uint64_t kSombraMinimoCiclos = 300;
// GPU timestamps per work slot (16 bytes each in the core's report block) and the end-of-work category.
constexpr uint32_t kMarcasPorRanura = 128;
constexpr uint8_t kGpuFin = 0xFF;

// With 4-byte texels, DesplazamientoMosaico2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (the Vulkan path's TablaMosaico2DTexel4).
const std::array<uint16_t, 1024>& TablaMosaico2DTexel4() {
  static const std::array<uint16_t, 1024> tabla = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(DesplazamientoMosaico2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return tabla;
}

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fijo16p8(float valor) {
  if (!(std::abs(valor) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double escalado = std::clamp(double(valor) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(escalado));
}

int32_t ExtenderSigno15(uint32_t valor) { return int32_t(valor << 17) >> 17; }

// fc3-switch: bytes per texel of a resolved texture as the guest lays it out (the tile offsets of a resolve into
// part of it, ObtenerResuelta's caller). The host formats a resolve can get; 4 for the rest.
uint32_t BytesPorTexelResuelta(DkImageFormat formato) {
  switch (formato) {
    case DkImageFormat_RGBA16_Float:
    case DkImageFormat_RGBA16_Unorm:
    case DkImageFormat_RGBA16_Snorm:
    case DkImageFormat_RG32_Float:
      return 8;
    case DkImageFormat_RGBA32_Float:
      return 16;
    case DkImageFormat_R16_Float:
    case DkImageFormat_R16_Unorm:
    case DkImageFormat_RGB565_Unorm:
      return 2;
    case DkImageFormat_R8_Unorm:
      return 1;
    default:
      return 4;
  }
}

DkImageView VistaCompleta(const DkImage& imagen) {
  DkImageView v;
  dkImageViewDefaults(&v, &imagen);
  return v;
}

std::filesystem::path RutaCache() { return rex::filesystem::GetExecutableFolder() / "cache" / "nfsmw_dksh.bin"; }

// ---- The output pass's shaders (GLSL for uam). The same work as the Vulkan path's nfsmw_salida_rampa_gamma.frag
// (see it for the reasons): a full-screen triangle, the source's texel, and the game's ramp.
constexpr const char* kVsSalida = R"(#version 460
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

// The ramp is a 256x1 texture, not a uniform array: each pixel indexes it with its own color, and a uniform array
// indexed by a different value in every lane of a warp is read one lane at a time (the constant cache serializes
// divergent addresses). It cost 2.3-2.5 ms per frame at 307.2 MHz.
constexpr const char* kFsRampa = R"(
layout(binding = 0) uniform sampler2D origen;
layout(binding = 1) uniform sampler2D tabla;
layout(std140, binding = 0) uniform Rampa {
  vec4 mezcla;   // saturacion, vibracion, 1 / gamma (GRADUACION)
  vec4 efectos;  // vineta, lineas (GRADUACION)
  ivec4 desplazamiento;
  vec4 inverso;
} rampa;
layout(location = 0) out vec4 color;

const vec3 kPesosLuma = vec3(0.299, 0.587, 0.114);

float Luma(vec2 uv) { return dot(textureLod(origen, uv, 0.0).rgb, kPesosLuma); }

vec3 Fxaa(vec2 uv) {
  vec2 paso = rampa.inverso.xy;
  vec3 rgb_centro = textureLod(origen, uv, 0.0).rgb;
  float luma_centro = dot(rgb_centro, kPesosLuma);
  float luma_no = Luma(uv + vec2(-paso.x, -paso.y));
  float luma_ne = Luma(uv + vec2(paso.x, -paso.y));
  float luma_so = Luma(uv + vec2(-paso.x, paso.y));
  float luma_se = Luma(uv + vec2(paso.x, paso.y));
  float luma_min = min(luma_centro, min(min(luma_no, luma_ne), min(luma_so, luma_se)));
  float luma_max = max(luma_centro, max(max(luma_no, luma_ne), max(luma_so, luma_se)));
  if (luma_max - luma_min < max(0.0833, luma_max * 0.166)) {
    return rgb_centro;
  }
  vec2 direccion = vec2(-((luma_no + luma_ne) - (luma_so + luma_se)), (luma_no + luma_so) - (luma_ne + luma_se));
  float reduccion = max((luma_no + luma_ne + luma_so + luma_se) * (0.25 * 0.125), 1.0 / 128.0);
  float inversa_min = 1.0 / (min(abs(direccion.x), abs(direccion.y)) + reduccion);
  direccion = clamp(direccion * inversa_min, vec2(-8.0), vec2(8.0)) * paso;
  vec3 rgb_a = 0.5 * (textureLod(origen, uv + direccion * (1.0 / 3.0 - 0.5), 0.0).rgb +
                      textureLod(origen, uv + direccion * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (textureLod(origen, uv + direccion * -0.5, 0.0).rgb +
                                     textureLod(origen, uv + direccion * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, kPesosLuma);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - rampa.desplazamiento.xy;
  vec3 texel;
#if FXAA
  texel = Fxaa((vec2(uvec2(pixel)) + 0.5) * rampa.inverso.xy);
#elif EXACTA
  texel = texelFetch(origen, pixel, 0).rgb;
#else
  texel = textureLod(origen, (vec2(uvec2(pixel)) + 0.5) * rampa.inverso.xy, 0.0).rgb;
#endif
#if RAMPA
  ivec3 i = ivec3(texel * 255.0 + 0.5);
  vec3 c = vec3(texelFetch(tabla, ivec2(i.r, 0), 0).r, texelFetch(tabla, ivec2(i.g, 0), 0).g,
                texelFetch(tabla, ivec2(i.b, 0), 0).b);
#else
  vec3 c = texel;
#endif
#if GRADUACION
  float luma = dot(max(c, 0.0), vec3(0.2126, 0.7152, 0.0722));
  c = mix(vec3(luma), c, rampa.mezcla.x);
  float saturado = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
  c = mix(vec3(luma), c, 1.0 + rampa.mezcla.y * (1.0 - clamp(saturado, 0.0, 1.0)));
  c = pow(max(c, vec3(0.0001)), vec3(rampa.mezcla.z));
  vec2 uv = (vec2(uvec2(pixel)) + 0.5) * rampa.inverso.xy;
  float v = rampa.efectos.x;
  c *= (1.0 - v * max(0.0, 1.0 - uv.y / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.y) / 0.42)) *
       (1.0 - v * max(0.0, 1.0 - uv.x / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.x) / 0.42));
  if ((uint(pixel.y) % 3u) == 0u) {
    c *= 1.0 - rampa.efectos.y * 0.7;
  }
  c = clamp(c, 0.0, 1.0);
#endif
  color = vec4(c, 1.0);
}
)";

// The 3D engine's copies (Copiar3D): the source's texel at this pixel plus the offset, as color or as depth.
constexpr const char* kFsCopiaColor = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
  ivec4 modos;   // kFsCopiaSigno's; unused here
  vec4 escala;   // 2^copy_dest_exp_bias
} copia;
layout(location = 0) out vec4 color;
void main() {
  color = texelFetch(origen, ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy, 0) * copia.escala;
}
)";
// The copy out of a k_2_10_10_10_FLOAT target. The Xbox 360 stores it as 7e3 floats (unsigned, at most 31.875) with a
// 2-bit alpha: a pixel shader's negative, NaN or larger output is clamped when written. The renderer keeps the target
// as RGBA16_Float, which holds them all, so they are clamped here, where the game reads the scene back: Gears of War 2's
// scene had -0.17 and 74.5 in it, and its post-processing and motion blur turned them into black smears.
constexpr const char* kFsCopia7e3 = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
  ivec4 modos;
  vec4 escala;
} copia;
layout(location = 0) out vec4 color;
void main() {
  vec4 t = texelFetch(origen, ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy, 0);
  vec4 tope = vec4(31.875, 31.875, 31.875, 1.0);
  if (copia.desplazamiento.z != 0) {
    // Convert each guest 7e3 value to its stored ten bits, then sample those bits as UNORM.
    vec3 x = clamp(t.rgb, 0.0, 31.875);
    x = mix(x, vec3(0.0), isnan(x));
    vec3 e = max(floor(log2(max(x, vec3(0.25)))), vec3(-2.0));
    vec3 bits = roundEven(x * exp2(7.0 - e)) + 128.0 * (e + 2.0);
    color = vec4(bits / 1023.0, roundEven(clamp(t.a, 0.0, 1.0) * 3.0) / 3.0) * copia.escala;
    return;
  }
  // Clamped in EDRAM units, then the resolve's exponent bias (Gears of War 2: scene x 8 in EDRAM, resolved with -3).
  color = vec4(greaterThan(t, vec4(0.0))) * min(t, tope) * copia.escala;  // NaN compares false: 0
}
)";
// The same copy with each channel's TextureSign applied: 1 reads the 8-bit value as signed, 2 as biased (2x - 1).
constexpr const char* kFsCopiaSigno = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
  ivec4 modos;
  vec4 escala;  // 2^exp_adjust of the fetch that reads it (the hardware scales what it returns)
} copia;
layout(location = 0) out vec4 color;
void main() {
  vec4 t = texelFetch(origen, ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy, 0);
  vec4 b = floor(t * 255.0 + 0.5);
  vec4 firmado = max(mix(b, b - 256.0, greaterThanEqual(b, vec4(128.0))) / 127.0, vec4(-1.0));
  color = mix(t, firmado, equal(copia.modos, ivec4(1)));
  color = mix(color, t * 2.0 - 1.0, equal(copia.modos, ivec4(2)));
  // 3, 4: a k_16_16 target (fixed point, -32..32, held here as is) read as a k_16_16 texture: its 16 bits are the value
  // x 1024 (two's complement), so unsigned reads give those bits / 65535 and signed reads the value / 32.
  color = mix(color, mod(floor(t * 1024.0 + 0.5) + 65536.0, 65536.0) / 65535.0, equal(copia.modos, ivec4(3)));
  color = mix(color, clamp(t / 32.0, -1.0, 1.0), equal(copia.modos, ivec4(4)));
  color *= copia.escala;
}
)";
constexpr const char* kFsCopiaProfundidad = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
} copia;
void main() {
  gl_FragDepth = texelFetch(origen, ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy, 0).r;
}
)";
// fc3-switch (PROTOTYPE): a depth target copied as the bytes of its D24S8 words (what a k_8_8_8_8 fetch of a depth
// resolve returns on the Xbox 360): X = stencil, Y/Z/W = depth bits 0-7/8-15/16-23. modos.x 1: k_24_8_FLOAT target,
// depth encoded as 20e4 (Xenia's PreClampedDepthTo20e4); 0: 24-bit unorm.
constexpr const char* kFsZComoColor = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(binding = 1) uniform usampler2D estencil;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
  ivec4 modos;
  vec4 escala;
} copia;
layout(location = 0) out vec4 color;
uint Profundidad20e4(float z) {
  uint f = floatBitsToUint(z);
  if (f < 0x38800000u) {
    uint e = 113u - (f >> 23);
    f = e < 24u ? (((f & 0x7FFFFFu) | 0x800000u) >> e) : 0u;
  } else {
    f += 0xC8000000u;
  }
  return ((f + 3u + ((f >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy;
  float z = clamp(texelFetch(origen, p, 0).r, 0.0, 1.0);
  uint s = texelFetch(estencil, p, 0).r & 0xFFu;
  uint d = copia.modos.x != 0 ? Profundidad20e4(z) : uint(z * 16777215.0 + 0.5);
  color = vec4(uvec4(s, d & 0xFFu, (d >> 8) & 0xFFu, d >> 16)) / 255.0;
}
)";

// fc3-switch (PROTOTYPE): the reverse, a k_8_8_8_8 target holding D24S8 words (R = stencil, G/B/A = depth bits
// 0-7/8-15/16-23) written into depth and stencil, as the EDRAM bytes are shared on the Xbox 360. modos.x 1: the depth
// target is k_24_8_FLOAT (20e4, Xenia's Float20e4To32); 0: 24-bit unorm.
constexpr const char* kFsColorAZ = R"(#version 460
layout(binding = 0) uniform sampler2D origen;
layout(std140, binding = 0) uniform Copia {
  ivec4 desplazamiento;
  ivec4 modos;
  vec4 escala;
} copia;
float Desde20e4(uint v) {
  uint m = v & 0xFFFFFu;
  int e = int(v >> 20);
  if (v == 0u) {
    return 0.0;
  }
  if (e == 0) {
    int lz = 20 - findMSB(m);
    e = 1 - lz;
    m = (m << uint(lz)) & 0xFFFFFu;
  }
  return uintBitsToFloat((uint(e + 112) << 23) | (m << 3));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) + copia.desplazamiento.xy;
  // Xenos color and depth address the two 40-sample halves of each 80-sample tile oppositely.
  // The native alias path represents 32-bit, single-sample EDRAM images at the same pitch.
  if (copia.modos.z != 0) p.x += (p.x % 80) < 40 ? 40 : -40;
  uvec4 b = uvec4(texelFetch(origen, p, 0) * 255.0 + 0.5);
  uint d = b.y | (b.z << 8) | (b.w << 16);
  gl_FragDepth = copia.modos.x != 0 ? clamp(Desde20e4(d), 0.0, 1.0) : float(d) / 16777215.0;
  if (copia.modos.y != 0 && (b.x & uint(copia.modos.y)) == 0u) discard;
}
)";

constexpr uint32_t kRampaExacta = 1, kRampaGraduacion = 2, kRampaFxaa = 4;
constexpr uint32_t kBytesUboRampa = 4 * 16;  // mezcla, efectos, desplazamiento, inverso (the ramp is a texture)

constexpr uint32_t kBytesVolcado = 48u << 20;  // the frame dump's read-back block (PedirVolcado)

float MedioAFloat(uint16_t h) {
  const uint32_t signo = uint32_t(h >> 15) << 31, exponente = (h >> 10) & 0x1F, mantisa = h & 0x3FF;
  if (exponente == 0) {
    const float v = std::ldexp(float(mantisa), -24);
    return signo ? -v : v;
  }
  const uint32_t f = exponente == 31 ? (signo | 0x7F800000u | (mantisa << 13))
                                     : (signo | ((exponente + 112) << 23) | (mantisa << 13));
  float v;
  std::memcpy(&v, &f, 4);
  return v;
}

// A 24-bit BMP of `bgr` (width * height * 3, top row first).
void EscribirBmp(const std::filesystem::path& ruta, uint32_t ancho, uint32_t alto, const std::vector<uint8_t>& bgr) {
  const uint32_t fila = (ancho * 3 + 3) & ~3u;
  uint8_t cabecera[54] = {'B', 'M'};
  auto u32 = [&](size_t at, uint32_t v) { std::memcpy(cabecera + at, &v, 4); };
  u32(2, 54 + fila * alto);
  u32(10, 54);
  u32(14, 40);
  u32(18, ancho);
  u32(22, alto);
  cabecera[26] = 1;
  cabecera[28] = 24;
  u32(34, fila * alto);
  // The whole file in memory and one write: small writes to the SD card take 10-50 ms each (720 rows: seconds).
  std::vector<uint8_t> archivo(sizeof(cabecera) + size_t(fila) * alto, 0);
  std::memcpy(archivo.data(), cabecera, sizeof(cabecera));
  for (uint32_t y = 0; y < alto; ++y) {
    std::memcpy(archivo.data() + sizeof(cabecera) + size_t(y) * fila, bgr.data() + size_t(alto - 1 - y) * ancho * 3,
                size_t(ancho) * 3);
  }
  if (FILE* f = std::fopen(ruta.string().c_str(), "wb")) {
    std::fwrite(archivo.data(), 1, archivo.size(), f);
    std::fclose(f);
  }
}

}  // namespace

// ---- Construction

RenderDk::RenderDk(deko::ProveedorDeko3d& proveedor, rex::memory::Memory* memoria, rex::x360::RenderHooks& hooks)
    : proveedor_(proveedor), compilador_(proveedor.compilador()), memoria_(memoria), hooks_(hooks) {
  deko::ConfigNucleo config;
  config.ranuras = kRanuras;
  config.mb_subida = uint32_t(REXCVAR_GET(nfsmw_dk_subida_mb));
  config.kb_comandos = 2048;
  config.kb_previos = 512;
  config.mb_codigo = 16;
  config.mb_trozo_imagenes = 32;
  config.descriptores_imagen = 8192;
  config.descriptores_sampler = 1024;
  nucleo_.Crear(proveedor_.dispositivo(), config);
  geometria_.Crear(nucleo_, uint32_t(REXCVAR_GET(nfsmw_dk_geometria_mb)), REXCVAR_GET(nfsmw_dk_geometria_muestreo));
  descriptores_por_liberar_.resize(kRanuras);
  lecturas_.resize(kRanuras);
  marcas_.resize(kRanuras);
  lectura_bloque_ = nucleo_.CrearBloqueLectura(kRanuras * kBytesLecturaRanura, "lecturas de vuelta (por ranura)");
  lectura_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(lectura_bloque_));
  lectura_gpu_ = dkMemBlockGetGpuAddr(lectura_bloque_);
  oclusion_bloque_ = nucleo_.CrearBloqueLectura(kParesOclusion * 32, "consultas de oclusion");
  oclusion_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(oclusion_bloque_));
  oclusion_gpu_ = dkMemBlockGetGpuAddr(oclusion_bloque_);
  if (REXCVAR_GET(nfsmw_dk_perfil_gpu_s) > 0) {
    perfil_bloque_ = nucleo_.CrearBloqueLectura(kMarcasPerfil * 32, "perfil de la GPU por dibujo");
    perfil_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(perfil_bloque_));
    perfil_gpu_ = dkMemBlockGetGpuAddr(perfil_bloque_);
    perfil_marcas_.reserve(kMarcasPerfil);
    perfil_ns_ = deko::AhoraNs();
    copias_bloque_ = nucleo_.CrearBloqueLectura(kMarcasCopia * 32, "perfil de la GPU por copia");
    copias_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(copias_bloque_));
    copias_gpu_ = dkMemBlockGetGpuAddr(copias_bloque_);
    marcas_copia_.reserve(kMarcasCopia);
  }
  vigia_ = std::thread([this] { Vigia(); });
  rampa_gamma_ = REXCVAR_GET(nfsmw_nativo_rampa_gamma);
  for (uint32_t i = 0; i < 256; ++i) {
    const uint16_t v = uint16_t((i * 1023 + 127) / 255);
    rampa_juego_[i] = {v, v, v};
  }
  RecalcularRampa();

  // The DKSH cache: the console's own (cache/, what it compiled while playing) and the one the PC tool made next to the
  // NRO (shaders/build_library.sh), merged. Loading only the first, a library rebuilt on the PC had none of its new or
  // changed shaders: each one was compiled on the console when first drawn, 4-9 s each for Dead Space 2's particles.
  std::string motivo;
  for (const std::filesystem::path& ruta :
       {RutaCache(), rex::filesystem::GetExecutableFolder() / "shadercache_dksh.bin"}) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
      continue;
    }
    std::vector<uint8_t> datos((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    deko::CacheDksh leida;
    if (!leida.Cargar(datos, motivo)) {
      REXLOG_WARN("[deko3d] cache de DKSH {} no se usa: {}", ruta.string(), motivo);
      continue;
    }
    size_t nuevas = 0;
    const bool de_la_consola = ruta == RutaCache();
    for (const auto& [clave, dksh] : leida.entradas()) {
      if (de_la_consola) {
        cache_consola_.Anadir(clave, dksh);
      }
      if (!cache_.Buscar(clave)) {
        cache_.Anadir(clave, dksh);
        ++nuevas;
      }
    }
    REXLOG_INFO("[deko3d] cache de DKSH: {} entradas de {} ({} nuevas)", leida.size(), ruta.string(), nuevas);
  }
  if (!cache_.size()) {
    REXLOG_WARN("[deko3d] sin cache de DKSH: cada shader se compila en la consola la primera vez que se usa");
  }
  cache_guardada_ns_ = deko::AhoraNs();

  const deko::Compilador::Resultado vacio = compilador_.Compilar(false, "#version 460\nvoid main() {}\n");
  if (vacio.ok) {
    fs_vacio_valido_ = nucleo_.CargarShader(fs_vacio_, vacio.dksh, "fragmentos vacio");
  }
  if (!fs_vacio_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar el pixel shader vacio: {}", vacio.log);
  }
  {
    // fc3-switch 2026-11-08 DIAGNOSTIC (fc3_dk_color_falso): writes the draw's identity (uniform binding 5) to every
    // color target, to find which draw paints a pixel from a resolve dump.
    const deko::Compilador::Resultado falso = compilador_.Compilar(
        false,
        R"(#version 460
layout(std140, binding = 5) uniform Id { vec4 id; } u;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() { o0 = u.id; o1 = u.id; o2 = u.id; o3 = u.id; }
)");
    fs_falso_valido_ = falso.ok && nucleo_.CargarShader(fs_falso_, falso.dksh, "color falso");
    if (!fs_falso_valido_) {
      REXLOG_ERROR("[deko3d] no se pudo compilar el shader de color falso: {}", falso.log);
    }
  }
  {
    // fc3-switch (PROTOTYPE, 2026-11-02): rectangle lists (Far Cry 3: ~100 a frame, its post-process passes; rejected
    // before, so the screen kept the last video frame). Each 3 vertices are a rectangle; the 4th corner is
    // v1 + v2 - v0 when v0 and v2 share x (left aligned), else v0 + v2 - v1, as Xenia's rect_list geometry shader does.
    // Every interpolator the translated vertex shaders can write (locations 0-21: TEXCOORD0-15, COLOR0-5) is carried.
    constexpr int kSalidas = 22;
    std::string g = "#version 460\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 4) out;\n";
    for (int i = 0; i < kSalidas; ++i) {
      g += fmt::format("layout(location = {0}) in vec4 e{0}[];\nlayout(location = {0}) out vec4 s{0};\n", i);
    }
    auto emitir = [&](const std::string& p, const std::function<std::string(int)>& valor) {
      std::string t = "  gl_Position = " + p + ";\n";
      for (int i = 0; i < kSalidas; ++i) t += fmt::format("  s{} = {};\n", i, valor(i));
      return t + "  EmitVertex();\n";
    };
    auto vertice = [&](int k) {
      return emitir(fmt::format("gl_in[{}].gl_Position", k), [k](int i) { return fmt::format("e{}[{}]", i, k); });
    };
    auto cuarto = [&](int a, int b, int c) {  // a + b - c
      return emitir(fmt::format("gl_in[{0}].gl_Position + gl_in[{1}].gl_Position - gl_in[{2}].gl_Position", a, b, c),
                    [a, b, c](int i) { return fmt::format("e{0}[{1}] + e{0}[{2}] - e{0}[{3}]", i, a, b, c); });
    };
    g += "void main() {\n  if (gl_in[0].gl_Position.x == gl_in[2].gl_Position.x) {\n";
    g += vertice(0) + vertice(1) + vertice(2) + cuarto(1, 2, 0);
    g += "  } else {\n";
    g += vertice(0) + vertice(1) + cuarto(0, 2, 1) + vertice(2);
    g += "  }\n  EndPrimitive();\n}\n";
    const deko::Compilador::Resultado r = compilador_.CompilarGeometria(g);
    if (r.ok) {
      gs_rectangulos_valido_ = nucleo_.CargarShader(gs_rectangulos_, r.dksh, "rectangulos");
    }
    if (!gs_rectangulos_valido_) {
      REXLOG_ERROR("[deko3d] no se pudo compilar el geometry shader de rectangulos: {}", r.log);
    }
  }
  CrearShadersSalida();
  AsegurarTrabajo();
  CrearVacias();
  REXLOG_INFO("[deko3d] destinos: etapa P2 (videos, menus y titulo): {} MB de subida por ranura, compresion de color "
              "{}, rampa de gamma {}",
              config.mb_subida, REXCVAR_GET(nfsmw_dk_compresion_color) ? "SI" : "no", rampa_gamma_ ? "SI" : "no");
}

RenderDk::~RenderDk() {
  vigia_fin_ = true;
  if (vigia_.joinable()) {
    vigia_.join();
  }
  if (nucleo_.TrabajoAbierto()) {
    CerrarTrabajo();
  }
  nucleo_.EsperarGpu();
  GuardarCacheSiHaceFalta(true);
  if (guardando_.joinable()) {
    guardando_.join();
  }
  geometria_.Destruir();
  nucleo_.Destruir();
}

// ---- Frames and work slots

void RenderDk::AsegurarTrabajo() {
  if (nucleo_.TrabajoAbierto()) {
    return;
  }
  nucleo_.EmpezarTrabajo(++trabajo_);
  // The descriptors freed the last time this slot was recorded: its fence has just been waited for. The same for
  // the read-backs recorded then, if Presentar has not written them yet.
  auto& libres = descriptores_por_liberar_[trabajo_ % kRanuras];
  descriptores_libres_.insert(descriptores_libres_.end(), libres.begin(), libres.end());
  libres.clear();
  EscribirLecturas(uint32_t(trabajo_ % kRanuras));
  lecturas_[trabajo_ % kRanuras].trabajo = trabajo_;
  LeerMarcasGpu(uint32_t(trabajo_ % kRanuras));
  dedupe_.NuevoFotograma(trabajo_);  // a new upload ring: the copies of the previous works are not here
  geometria_.NuevoTrabajo(trabajo_);
  InicioDeTrabajo();
}

void RenderDk::InicioDeTrabajo() {
  DkCmdBuf cmd = nucleo_.comandos();
  marcas_[trabajo_ % kRanuras].clear();
  MarcarGpu(kGpuOtros);  // the work's start
  // Another command list: what the previous one drew may still be in flight when a transfer of this one starts.
  trabajo_3d_sin_barrera_ = true;
  transferencia_sin_barrera_ = false;
  rop_sin_barrera_ = false;  // the Fragments barrier above made it visible
  copia3d_pendiente_ = false;
  // What an earlier submission sampled or wrote (the previous output pass, the presenter's paint of the same image)
  // is done before this one's draws; the caches forget what the CPU rewrote in this slot's memory.
  dkCmdBufBarrier(cmd, DkBarrier_Fragments,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
  dkCmdBufTiledCacheOp(cmd, DkTiledCacheOp_Disable);
  nucleo_.BindDescriptores();
  OlvidarEstado();
}

void RenderDk::CerrarTrabajo() {
  if (!nucleo_.TrabajoAbierto()) {
    return;
  }
  EmitirCieloPendiente();  // the deferred sky belongs to this work's pass
  // The fence is released by the 3D engine: a transfer still running on the copy engine is waited for first (the
  // read-backs are read when the fence has passed).
  if (transferencia_sin_barrera_) {
    BarreraTransferencia();
    transferencia_sin_barrera_ = false;
  }
  MarcarGpu(kGpuFin);
  nucleo_.TerminarTrabajo();
}

void RenderDk::MarcarGpu(uint32_t categoria) {
  if (!nucleo_.TrabajoAbierto()) {
    return;
  }
  std::vector<uint8_t>& m = marcas_[trabajo_ % kRanuras];
  // The same category goes on; past the limit the last segment absorbs the rest (the end always fits).
  if (categoria != kGpuFin && ((!m.empty() && m.back() == categoria) || m.size() + 2 > kMarcasPorRanura)) {
    return;
  }
  dkCmdBufReportCounter(nucleo_.comandos(), DkCounter_Timestamp,
                        nucleo_.ConsultasGpu() + ((trabajo_ % kRanuras) * kMarcasPorRanura + m.size()) * 16);
  m.push_back(uint8_t(categoria));
}

void RenderDk::LeerMarcasGpu(uint32_t ranura) {
  std::vector<uint8_t>& m = marcas_[ranura];
  if (m.size() < 2 || m.back() != kGpuFin) {
    m.clear();
    return;
  }
  const uint8_t* const base = nucleo_.ConsultasCpu() + size_t(ranura) * kMarcasPorRanura * 16;
  auto marca = [&](size_t i) {
    uint64_t t = 0;
    std::memcpy(&t, base + i * 16 + 8, sizeof(t));
    return t;
  };
  uint64_t previa = marca(0);
  const uint64_t inicio = previa;
  for (size_t i = 1; i < m.size(); ++i) {
    const uint64_t t = marca(i);
    if (t >= previa && m[i - 1] < kGpuCategorias) {
      gpu_categorias_ns_[m[i - 1]] += uint64_t(double(t - previa) * deko::kNsPorUnidadGpu);
    }
    previa = t;
  }
  if (previa > inicio) {
    gpu_ns_ += uint64_t(double(previa - inicio) * deko::kNsPorUnidadGpu);
    ++gpu_trabajos_;
  }
  // The GPU idle between the previous work's end and this one's start; longer than 100 ms is a pause, not a gap.
  if (marca_fin_previa_ && inicio > marca_fin_previa_) {
    const double hueco = double(inicio - marca_fin_previa_) * deko::kNsPorUnidadGpu;
    if (hueco < 100e6) {
      gpu_categorias_ns_[kGpuHuecoEntreTrabajos] += uint64_t(hueco);
    }
  }
  marca_fin_previa_ = previa;
  m.clear();
}

bool RenderDk::CambiarDeRanura() {
  ++cambios_ranura_;
  CerrarTrabajo();
  AsegurarTrabajo();
  return true;
}

void RenderDk::OlvidarEstado() {
  g_ = Grabado{};
  constantes_trabajo_ = UINT64_MAX;
  compartidas_trabajo_ = UINT64_MAX;
}

void RenderDk::BarreraTransferencia() {
  dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full, DkInvalidateFlags_Image);
  ++barreras_;
}

void RenderDk::AntesDeTransferir() {
  if (trabajo_3d_sin_barrera_) {
    BarreraTransferencia();  // the draws that wrote the source, and the ones that sampled the destination
    trabajo_3d_sin_barrera_ = false;
  }
  transferencia_sin_barrera_ = true;
}

void RenderDk::AntesDe3D() {
  if (transferencia_sin_barrera_) {
    // The transfer is done and the texture caches forget the old texels (and ZCULL, if a depth changed).
    dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full,
                    DkInvalidateFlags_Image | (invalidar_zcull_ ? DkInvalidateFlags_Zcull : 0u));
    ++barreras_;
    transferencia_sin_barrera_ = false;
    invalidar_zcull_ = false;
    copia3d_pendiente_ = false;
  } else if (copia3d_pendiente_) {
    // A 3D copy: what it wrote is sampled from now on, and its source may be written (cleared, drawn on) next.
    dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Fragments,
                    DkInvalidateFlags_Image | (invalidar_zcull_ ? DkInvalidateFlags_Zcull : 0u));
    ++barreras_fragmentos_;
    copia3d_pendiente_ = false;
    invalidar_zcull_ = false;
  } else if (invalidar_zcull_) {
    dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_None, DkInvalidateFlags_Zcull);
    invalidar_zcull_ = false;
  }
  trabajo_3d_sin_barrera_ = true;
  rop_sin_barrera_ = true;
}

namespace {
// fc3_dk_copias_sin_barrera: image memory read and written by the 3D copies since the last barrier that covered them.
struct RangoCopia {
  DkMemBlock bloque;
  uint32_t offset, bytes;
};
std::vector<RangoCopia> g_copias_escritas, g_copias_leidas;
uint64_t g_copias_barreras_evitadas = 0;
bool SolapaCopia(const std::vector<RangoCopia>& lista, const deko::BloqueImagen& m) {
  if (!m.bloque) {
    return true;  // unknown memory: assume a hazard
  }
  for (const RangoCopia& r : lista) {
    if (r.bloque == m.bloque && m.offset < r.offset + r.bytes && r.offset < m.offset + m.bytes) {
      return true;
    }
  }
  return false;
}
}  // namespace

bool RenderDk::Copiar3D(const ImagenDk& origen, uint32_t x0, uint32_t y0, uint32_t ancho, uint32_t alto,
                        ImagenDk& destino, uint32_t dx, uint32_t dy, const int32_t* modos_signo, bool limitar_7e3,
                        float escala, bool z_como_color, bool color_a_z, bool reinterpretar_7e3) {
  const bool z = destino.profundidad;
  limitar_7e3 = limitar_7e3 && !z && !modos_signo && fs_copia_7e3_valido_;
  if (color_a_z) {
    if (!z || origen.profundidad || !vs_salida_valido_ || !fs_color_a_z_valido_) {
      return false;
    }
  } else if (z_como_color) {
    if (z || !origen.profundidad || !vs_salida_valido_ || !fs_z_como_color_valido_) {
      return false;
    }
  } else if (!vs_salida_valido_ || !(z ? fs_copia_profundidad_valido_ : fs_copia_color_valido_) ||
             (modos_signo && (z || !fs_copia_signo_valido_))) {
    return false;
  }
  AsegurarTrabajo();
  EmitirCieloPendiente();  // the copy binds other targets
  const deko::Subida ubo = Reservar(color_a_z ? 9 * 256 : 256, DK_UNIFORM_BUF_ALIGNMENT);
  if (!ubo) {
    return false;
  }
  const int32_t desplazamiento[4] = {int32_t(x0) - int32_t(dx), int32_t(y0) - int32_t(dy), int32_t(reinterpretar_7e3), 0};
  std::memcpy(ubo.cpu, desplazamiento, sizeof(desplazamiento));
  if (modos_signo) {
    std::memcpy(ubo.cpu + sizeof(desplazamiento), modos_signo, 4 * sizeof(int32_t));
  }
  if (color_a_z) {
    const int32_t swap_columns = int32_t(REXCVAR_GET(fc3_color_depth_swap40));
    std::memcpy(ubo.cpu + 24, &swap_columns, sizeof(swap_columns));  // copia.modos.z, also copied to every bit pass.
  }
  const float escalas[4] = {escala, escala, escala, escala};
  std::memcpy(ubo.cpu + 32, escalas, sizeof(escalas));
  const uint32_t ranura = VistaDe(origen, 0x688, (z && !color_a_z) || z_como_color ? 0 /* RRRR */ : 0x688);
  const uint32_t ranura_estencil = z_como_color ? VistaDe(origen, 0x688, 0x688, DkImageFormat_None, true) : 0;
  if (z_como_color && !ranura_estencil) {
    return false;
  }
  DkCmdBuf cmd = nucleo_.comandos();
  MarcarGpu(kGpuCopias);
  const char* const tipo_copia = tipo_copia_;
  vigia_copia_.store(tipo_copia, std::memory_order_relaxed);
  tipo_copia_ = "3d";
  const int32_t marca_copia = PerfilCopiaInicio(cmd);
  if (transferencia_sin_barrera_) {
    BarreraTransferencia();  // a copy engine transfer wrote what this reads (or reads what this writes)
    transferencia_sin_barrera_ = false;
    copia3d_pendiente_ = false;
    rop_sin_barrera_ = false;
  }
  if (!copia3d_pendiente_) {
    // No copy since the last barrier (draws in between reset copia3d_pendiente_ with their own barrier).
    g_copias_escritas.clear();
    g_copias_leidas.clear();
  }
  const bool sin_barrera = copia3d_pendiente_ && !rop_sin_barrera_ && REXCVAR_GET(fc3_dk_copias_sin_barrera) &&
                           !invalidar_zcull_ && !SolapaCopia(g_copias_escritas, origen.memoria) &&
                           !SolapaCopia(g_copias_leidas, destino.memoria);
  if (sin_barrera) {
    // Only copies since the last barrier, none of them wrote what this reads or read what this writes. The next
    // draw or clear still waits for all of them (copia3d_pendiente_ stays set).
    ++g_copias_barreras_evitadas;
    if (g_copias_barreras_evitadas == 1 || (g_copias_barreras_evitadas % 100000) == 0) {
      REXLOG_INFO("[deko3d] fc3_dk_copias_sin_barrera: {} barriers between independent 3D copies left out",
                  g_copias_barreras_evitadas);
    }
  } else if (rop_sin_barrera_ || copia3d_pendiente_) {
    g_copias_escritas.clear();
    g_copias_leidas.clear();
    // The source was drawn (or copied into) since the last barrier: its writes must reach the texture units.
    dkCmdBufBarrier(cmd, DkBarrier_Fragments,
                    DkInvalidateFlags_Image | (invalidar_zcull_ ? DkInvalidateFlags_Zcull : 0u));
    ++barreras_fragmentos_;
    rop_sin_barrera_ = false;
    copia3d_pendiente_ = false;
    invalidar_zcull_ = false;
  }
  DkImageView vista;
  dkImageViewDefaults(&vista, &destino.imagen);
  if (z) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
  }
  const DkViewport vp = {float(dx), float(dy), float(ancho), float(alto), 0.0f, 1.0f};
  const DkScissor tijera = {dx, dy, ancho, alto};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  static const DkViewportSwizzle kIdentidad = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentidad, 1);
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  if (z) {
    cws.masks = 0;
  }
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = z;
  ds.depthWriteEnable = z;
  ds.depthCompareOp = DkCompareOp_Always;
  ds.stencilTestEnable = color_a_z;  // First draw initializes stencil to zero and imports depth.
  if (color_a_z) {
    ds.stencilFrontFailOp = ds.stencilFrontPassOp = ds.stencilFrontDepthFailOp = DkStencilOp_Replace;
    ds.stencilBackFailOp = ds.stencilBackPassOp = ds.stencilBackDepthFailOp = DkStencilOp_Replace;
    ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;
    dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, 0xFF, 0, 0xFF);
  }
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cws);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* shaders[2] = {&vs_salida_,
                                color_a_z     ? &fs_color_a_z_
                                : z_como_color ? &fs_z_como_color_
                                : z           ? &fs_copia_profundidad_
                                : modos_signo ? &fs_copia_signo_
                                : limitar_7e3 ? &fs_copia_7e3_
                                              : &fs_copia_color_};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, 256);
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(ranura, 0));
  if (z_como_color) {
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, dkMakeTextureHandle(ranura_estencil, 0));
  }
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  if (color_a_z) {
    // Write each source bit with a fixed stencil reference. Fragments without that bit discard.
    ds.depthTestEnable = ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(cmd, &ds);
    for (uint32_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 1u << bit;
      const uint32_t offset = (bit + 1) * 256;
      std::memcpy(ubo.cpu + offset, ubo.cpu, 48);
      std::memcpy(ubo.cpu + offset + 20, &mask, sizeof(mask));  // copia.modos.y
      dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu + offset, 256);
      dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, uint8_t(mask), uint8_t(mask), 0xFF);
      dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    }
  }
  if (g_copias_escritas.size() < 64) {
    g_copias_escritas.push_back({destino.memoria.bloque, destino.memoria.offset, destino.memoria.bytes});
    g_copias_leidas.push_back({origen.memoria.bloque, origen.memoria.offset, origen.memoria.bytes});
  } else {
    rop_sin_barrera_ = true;  // too many tracked copies: the next copy takes the barrier (and clears the lists)
  }
  PerfilCopiaFin(cmd, marca_copia,
                 {color_a_z ? "color->z" : z_como_color ? "z como color" : tipo_copia, ancho, alto,
                  uint32_t(origen.formato), destino.ancho, destino.alto, uint32_t(destino.formato), 0});
  ++copias_3d_;
  copia3d_pendiente_ = true;
  trabajo_3d_sin_barrera_ = true;  // for a copy engine transfer that comes next
  if (color_a_z && REXCVAR_GET(fc3_dk_alias_zcull) == 1) {
    invalidar_zcull_ = true;
  } else if (color_a_z && REXCVAR_GET(fc3_dk_alias_zcull) == 2) {
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Zcull);
    ++barreras_;
    copia3d_pendiente_ = false;
    invalidar_zcull_ = false;
  }
  OlvidarEstado();
  return true;
}

deko::Subida RenderDk::Reservar(uint32_t bytes, uint32_t alineacion) {
  AsegurarTrabajo();
  deko::Subida s = nucleo_.Reservar(bytes, alineacion);
  if (!s && bytes + alineacion <= (nucleo_.config().mb_subida << 20)) {
    CambiarDeRanura();
    s = nucleo_.Reservar(bytes, alineacion);
  }
  return s;
}

// ---- Images

bool RenderDk::CrearImagen(ImagenDk& imagen, DkImageType tipo, DkImageFormat formato, uint32_t ancho, uint32_t alto,
                           uint32_t capas, uint32_t niveles, uint32_t flags, const char* que) {
  if (!ancho || !alto || ancho > 16384 || alto > 16384) {
    REXLOG_WARN("[deko3d] {} de {}x{}: tamano imposible", que, ancho, alto);
    return false;
  }
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, nucleo_.dispositivo());
  m.type = tipo;
  m.flags = flags;
  m.format = formato;
  m.dimensions[0] = ancho;
  m.dimensions[1] = alto;
  m.dimensions[2] = tipo == DkImageType_Cubemap ? 6 : (tipo == DkImageType_3D ? capas : 1);
  m.mipLevels = std::max(niveles, 1u);
  const bool comprimido = formato >= DkImageFormat_RGB_BC1 && formato <= DkImageFormat_RG_BC5_Snorm;
  deko::ArreglarTamanoDeTesela(m, comprimido ? (alto + 3) / 4 : alto);
  DkImageLayout disposicion;
  dkImageLayoutInitialize(&disposicion, &m);
  const uint64_t bytes = dkImageLayoutGetSize(&disposicion);
  if (bytes >= (1ull << 31)) {
    return false;
  }
  imagen.memoria = nucleo_.ReservarImagen(uint32_t(bytes), dkImageLayoutGetAlignment(&disposicion));
  dkImageInitialize(&imagen.imagen, &disposicion, imagen.memoria.bloque, imagen.memoria.offset);
  imagen.tipo = tipo;
  imagen.formato = formato;
  imagen.ancho = ancho;
  imagen.alto = alto;
  imagen.alto_borrado = 0;       // nothing cleared yet: uninitialized memory
  imagen.banda_sucia = true;     // the whole target is stale until the first clear covers it
  imagen.borrado_pendiente = true;  // force a full clear on first use (fixes snow from uninitialized memory)
  imagen.capas = tipo == DkImageType_Cubemap ? 6 : capas;
  imagen.niveles = m.mipLevels;
  imagen.profundidad = formato == DkImageFormat_Z24S8 || formato == DkImageFormat_ZF32_X24S8;
  imagen.viva = true;
  imagen.serie = ++serie_imagenes_;
  return true;
}

void RenderDk::DestruirImagen(ImagenDk& imagen) {
  if (!imagen.viva) {
    return;
  }
  LiberarDescriptoresDe(imagen.serie);
  nucleo_.LiberarImagenLuego(imagen.memoria);
  imagen = ImagenDk{};
}

void RenderDk::PrepararDestino(ImagenDk& imagen) {
  AsegurarTrabajo();
  MarcarGpu(kGpuBorrados);
  // The values as the game's clears write them: color 0, and depth 1.0 with stencil 0 as RB_DEPTH_CLEAR.
  borrado_previo_[imagen.serie] = {imagen.profundidad ? uint64_t(0xFFFFFF00u) : 0, dibujados_};
  EmitirCieloPendiente();
  AntesDe3D();
  DkCmdBuf cmd = nucleo_.comandos();
  const DkImageView vista = VistaCompleta(imagen.imagen);
  const DkScissor tijera = {0, 0, imagen.ancho, imagen.alto};
  imagen.alto_borrado = imagen.alto;
  imagen.banda_sucia = false;
  imagen.borrado_z = 1.0f;
  imagen.borrado_estencil = 0;
  imagen.borrado_solo_estencil = false;
  std::fill(std::begin(imagen.borrado_color), std::end(imagen.borrado_color), 0.0f);
  if (imagen.profundidad) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &vista);
    dkCmdBufSetScissors(cmd, 0, &tijera, 1);
    dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
  } else {
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
    dkCmdBufSetScissors(cmd, 0, &tijera, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
  }
  OlvidarEstado();
}

// Render target formats: 8_8_8_8 (and its gamma form) as before; Dead Space 2 also draws into 32_FLOAT (its 64x8
// exposure target) and the float and 10-bit formats are mapped the same way. 2_10_10_10_FLOAT has no host
// equivalent with alpha: 16-bit float keeps its range.
bool FormatoDestinoColorDk(uint32_t formato_destino, DkImageFormat& host) {
  switch (formato_destino) {
    case 0:   // k_8_8_8_8
    case 1:   // k_8_8_8_8_GAMMA
      host = DkImageFormat_RGBA8_Unorm;
      return true;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      host = DkImageFormat_RGB10A2_Unorm;
      return true;
    case 3:   // k_2_10_10_10_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    case 7:   // k_16_16_16_16_FLOAT
      host = DkImageFormat_RGBA16_Float;
      return true;
    case 6:   // k_16_16_FLOAT
    case 4:   // k_16_16 (Xenos: fixed point, -32..32; a half float holds that range)
      host = DkImageFormat_RG16_Float;
      return true;
    case 5:   // k_16_16_16_16 (the same, four channels: Gears of War 2's velocity and light targets)
      host = DkImageFormat_RGBA16_Float;
      return true;
    case 14:  // k_32_FLOAT
      host = DkImageFormat_R32_Float;
      return true;
    case 15:  // k_32_32_FLOAT
      host = DkImageFormat_RG32_Float;
      return true;
    default:
      return false;
  }
}

// Resolve (copy destination) formats. 5_6_5 is kept as RGBA8: a resolved texture is sampled from its host image, not
// from guest memory, so only the values matter (Dead Space 2 resolves into 5_6_5 and samples it back).
bool FormatoResueltaDk(uint32_t formato_copia, DkImageFormat& host) {
  switch (formato_copia) {
    case 6:   // k_8_8_8_8
    case 8:   // k_8_8_8_8_A
    case 50:  // k_8_8_8_8_AS_16_16_16_16
    case 4:   // k_5_6_5
      host = DkImageFormat_RGBA8_Unorm;
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      host = DkImageFormat_RGB10A2_Unorm;
      return true;
    case 2:   // k_8 (the target's first channel: Dead Space 3 resolves single-channel masks)
      host = DkImageFormat_R8_Unorm;
      return true;
    case 31:  // k_16_16_FLOAT
    case 25:  // k_16_16 (resolved from a k_16_16 target)
      host = DkImageFormat_RG16_Float;
      return true;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
      host = DkImageFormat_RGBA16_Float;
      return true;
    case 36:  // k_32_FLOAT
      host = DkImageFormat_R32_Float;
      return true;
    case 37:  // k_32_32_FLOAT
      host = DkImageFormat_RG32_Float;
      return true;
    default:
      return false;
  }
}

ImagenDk* RenderDk::DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch) {
  if (!pitch) {
    Rechazar(7, "destino de render con pitch 0");
    return nullptr;
  }
  formato = uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(formato)));
  const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
  if (auto it = destinos_.find(clave); it != destinos_.end()) {
    return &it->second;
  }
  // The height from the pitch, as the Vulkan path (nfsmw_nativo_destinos.cpp, ObtenerDestino explains why).
  const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
  ImagenDk imagen;
  DkImageFormat formato_host = DkImageFormat_RGBA8_Unorm;
  if (!FormatoDestinoColorDk(formato, formato_host)) {
    Rechazar(100 + formato, "formato de destino de render todavia no soportado");
    return nullptr;
  }
  uint32_t flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  if (REXCVAR_GET(nfsmw_dk_compresion_color) && formato_host == DkImageFormat_RGBA8_Unorm) {
    flags |= DkImageFlags_HwCompression;
  }
  if (!CrearImagen(imagen, DkImageType_2D, formato_host, pitch, alto, 1, 1, flags, "destino de color")) {
    Rechazar(8, "no se pudo crear un destino de render");
    return nullptr;
  }
  REXLOG_INFO("[deko3d] destino de render base {:03X}, formato {}, {}x{}", base, formato, pitch, alto);
  ImagenDk& nueva = destinos_.emplace(clave, imagen).first->second;
  PrepararDestino(nueva);
  return &nueva;
}

ImagenDk* RenderDk::DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) {
  if (!pitch) {
    Rechazar(11, "sin destino de profundidad (pitch 0)");
    return nullptr;
  }
  const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
  if (auto it = profundidades_.find(clave); it != profundidades_.end()) {
    return &it->second;
  }
  const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
  ImagenDk imagen;
  // Both guest depth formats (24-bit fixed and float) as D24S8, as the Vulkan path does on the console. No
  // compression: untested on deko3d, and copies broke it on nouveau (HowToDeko3d.md, section 5.10).
  static const uint32_t flags_z =
      DkImageFlags_UsageRender | (REXCVAR_GET(nfsmw_dk_compresion_profundidad) ? DkImageFlags_HwCompression : 0u);
  if (!CrearImagen(imagen, DkImageType_2D, DkImageFormat_Z24S8, pitch, alto, 1, 1, flags_z, "destino de profundidad")) {
    Rechazar(12, "no se pudo crear un destino de profundidad");
    return nullptr;
  }
  REXLOG_INFO("[deko3d] destino de profundidad base {:03X}, formato {}, {}x{}", base, formato, pitch, alto);
  ImagenDk& nueva = profundidades_.emplace(clave, imagen).first->second;
  PrepararDestino(nueva);
  return &nueva;
}

// fc3-switch (PROTOTYPE, 2026-11-01): Far Cry 3 resolves to ever new addresses (47 different 1280x704 bases while the
// world loads) and nothing freed them: the images grew 222 -> 570 MB in 20 s and the game hung with the application
// memory at 99 %. Over the budget, the least recently used resolved textures not touched in this frame or the previous
// one go (their memory is freed once the GPU is done with it). A texture the game resolved long ago
// and samples again is created again empty: acceptable for now, to be checked on screen.
void RenderDk::ExpulsarResueltasViejas(uint32_t base_nueva) {
  const uint64_t presupuesto = uint64_t(REXCVAR_GET(nfsmw_dk_resueltas_mb)) << 20;
  if (!presupuesto) {
    return;
  }
  auto bytes = [](const Resuelta& r) { return uint64_t(r.imagen.ancho) * r.imagen.alto * 4; };
  uint64_t total = 0;
  for (const auto& par : resueltas_) total += bytes(par.second);
  for (const auto& par : resueltas_otras_) total += bytes(par.second);
  // fc3_dk_resueltas_fijas_kb (HACK): the small ones stay, while all of them fit in 64 MiB.
  const uint64_t fija = uint64_t(std::max(REXCVAR_GET(fc3_dk_resueltas_fijas_kb), 0)) << 10;
  uint64_t total_fijas = 0;
  if (fija) {
    for (const auto& par : resueltas_) total_fijas += bytes(par.second) <= fija ? bytes(par.second) : 0;
  }
  const bool proteger = fija && total_fijas <= (uint64_t(64) << 20);
  while (total > presupuesto) {
    auto vieja = resueltas_.end();
    for (auto it = resueltas_.begin(); it != resueltas_.end(); ++it) {
      if (proteger && bytes(it->second) <= fija) {
        continue;
      }
      if (it->first == base_nueva || it->second.uso >= uso_fotograma_anterior_ ||
          (frontal_.pendiente && frontal_.resuelta == &it->second)) {
        continue;
      }
      if (vieja == resueltas_.end() || it->second.uso < vieja->second.uso) vieja = it;
    }
    if (vieja == resueltas_.end()) {
      break;
    }
    total -= bytes(vieja->second);
    ++resueltas_expulsadas_;
    DestruirImagen(vieja->second.imagen);
    SombraMinimoResueltaEscrita(vieja->first);
    resueltas_.erase(vieja);
    ++generacion_texturas_;  // that address is sampled from guest memory again
  }
  if (resueltas_expulsadas_ && (resueltas_expulsadas_ & (resueltas_expulsadas_ - 1)) == 0) {
    REXLOG_INFO("[deko3d] texturas resueltas expulsadas por el presupuesto de {} MB: {} en total", presupuesto >> 20,
                resueltas_expulsadas_);
  }
}

RenderDk::Resuelta* RenderDk::ObtenerResuelta(uint32_t base, uint32_t ancho, uint32_t alto, uint32_t formato,
                                              bool intercambio_rb, bool profundidad) {
  if (!ancho || !alto) {
    Rechazar(9, "copia a una textura de tamano 0");
    return nullptr;
  }
  DkImageFormat formato_host = DkImageFormat_Z24S8;
  if (!profundidad && !FormatoResueltaDk(formato, formato_host)) {
    formato_host = DkImageFormat_RGBA8_Unorm;  // a depth copy's or an unknown guest format: as before
  }
  ++uso_resueltas_;
  if (auto it = resueltas_.find(base); it != resueltas_.end()) {
    Resuelta& r = it->second;
    if (r.imagen.ancho == ancho && r.imagen.alto == alto && r.imagen.formato == formato_host) {
      if (r.formato_guest != formato || r.imagen.intercambio_rb != intercambio_rb) {
        ++generacion_texturas_;  // changes the swizzle it is sampled with
      }
      r.formato_guest = formato;
      r.imagen.intercambio_rb = intercambio_rb;
      r.uso = uso_resueltas_;
      return &r;
    }
    // Another size or format at the same address: this one is kept for its own size (resueltas_otras_), and the
    // one of the new size, if it was kept, comes back without being created again.
    if (frontal_.pendiente && frontal_.resuelta == &r) {
      frontal_.pendiente = false;  // it moves: nothing to copy into
    }
    const auto vieja = std::make_tuple(base, r.imagen.ancho, r.imagen.alto, uint32_t(r.imagen.formato));
    if (auto o = resueltas_otras_.find(vieja); o != resueltas_otras_.end()) {
      DestruirImagen(o->second.imagen);
      resueltas_otras_.erase(o);
    }
    const auto nueva = std::make_tuple(base, ancho, alto, uint32_t(formato_host));
    for (;;) {
      // Room for the old one: the least recently used other size goes (never the one about to come back).
      uint32_t de_la_base = 0;
      auto menos_usada = resueltas_otras_.end();
      for (auto o = resueltas_otras_.lower_bound(std::make_tuple(base, 0u, 0u, 0u));
           o != resueltas_otras_.end() && std::get<0>(o->first) == base; ++o) {
        ++de_la_base;
        if (o->first != nueva && (menos_usada == resueltas_otras_.end() || o->second.uso < menos_usada->second.uso)) {
          menos_usada = o;
        }
      }
      if (de_la_base < kOtrasPorBase || menos_usada == resueltas_otras_.end()) {
        break;
      }
      DestruirImagen(menos_usada->second.imagen);
      resueltas_otras_.erase(menos_usada);
    }
    resueltas_otras_.emplace(vieja, std::move(r));
    resueltas_.erase(it);
    SombraMinimoResueltaEscrita(base);
    if (auto o = resueltas_otras_.find(nueva); o != resueltas_otras_.end()) {
      Resuelta vuelta = std::move(o->second);
      resueltas_otras_.erase(o);
      vuelta.formato_guest = formato;
      vuelta.imagen.intercambio_rb = intercambio_rb;
      vuelta.uso = uso_resueltas_;
      ++generacion_texturas_;  // that address is sampled from this image again
      return &resueltas_.emplace(base, std::move(vuelta)).first->second;
    }
  }
  ExpulsarResueltasViejas(base);
  Resuelta r;
  // A depth texture has the depth targets' flags (resolves without copy swap their images).
  const uint32_t flags =
      profundidad ? DkImageFlags_UsageRender |
                        (REXCVAR_GET(nfsmw_dk_compresion_profundidad) ? DkImageFlags_HwCompression : 0u)
                  : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  if (!CrearImagen(r.imagen, DkImageType_2D, formato_host, ancho, alto, 1, 1, flags, "textura resuelta")) {
    Rechazar(10, "no se pudo crear una textura resuelta");
    return nullptr;
  }
  r.formato_guest = formato;
  r.uso = uso_resueltas_;
  r.imagen.intercambio_rb = intercambio_rb;
  // Cleared once: a resolve that covers only part of it (a sub-rectangle, a narrower one) left the rest as whatever
  // the memory held, sampled as colored noise.
  r.imagen.borrado_z = 1.0f;
  r.imagen.borrado_estencil = 0;
  r.imagen.borrado_solo_estencil = false;
  std::fill(std::begin(r.imagen.borrado_color), std::end(r.imagen.borrado_color), 0.0f);
  r.imagen.borrado_pendiente = true;
  HacerBorradoPendiente(r.imagen);
  REXLOG_INFO("[deko3d] textura resuelta en {:08X}, {}x{}, formato {}{}", base, ancho, alto, formato,
              profundidad ? " (profundidad)" : "");
  ++generacion_texturas_;  // that address is now sampled from the resolved texture
  return &resueltas_.emplace(base, std::move(r)).first->second;
}

const ImagenDk* RenderDk::TexturaResuelta(uint32_t direccion, uint32_t ancho, uint32_t alto) {
  if (frontal_.pendiente && direccion == frontal_.base) {
    MaterializarFrontal();  // a draw samples the front buffer: it gets the copy now
  } else if (frontal_obsoleta_ && direccion == frontal_obsoleta_) {
    // Painted from its target and sampled afterwards: the texture holds an older frame. Copies from now on.
    frontal_obsoleta_ = 0;
    if (!frontal_apagado_) {
      frontal_apagado_ = true;
      REXLOG_WARN("[deko3d] frontal perezoso: un dibujo muestrea el frontal {:08X} despues del Swap; se copia siempre "
                  "a partir de ahora",
                  direccion);
    }
  }
  const auto it = resueltas_.find(direccion);
  const bool actual = it != resueltas_.end() && it->second.escrita;
  // A fetch of another size than the address's last resolve: the image of its size, if one was kept.
  if (ancho && alto && (!actual || it->second.imagen.ancho != ancho || it->second.imagen.alto != alto)) {
    for (auto o = resueltas_otras_.lower_bound(std::make_tuple(direccion, ancho, alto, 0u));
         o != resueltas_otras_.end() && std::get<0>(o->first) == direccion && std::get<1>(o->first) == ancho &&
         std::get<2>(o->first) == alto;
         ++o) {
      if (o->second.escrita) {
        o->second.uso = ++uso_resueltas_;
        return &o->second.imagen;
      }
    }
  }
  return actual ? &it->second.imagen : nullptr;
}

// ---- Copies (resolve and clear)

bool RenderDk::Rechazar(uint32_t causa, const char* texto) {
  ++rechazados_;
  ++causas_[causa];
  Avisar(causa, texto);
  return false;
}

void RenderDk::Avisar(uint32_t causa, const char* texto) {
  if (avisados_.emplace(causa, true).second) {
    REXLOG_WARN("[deko3d] {} (causa {})", texto, causa);
  }
}

// The copy rectangle, as the Vulkan path computes it (nfsmw_nativo_destinos.cpp, Rectangulo).
bool RenderDk::Rectangulo(const RegistrosCopia& reg, uint32_t pitch, int32_t& x0, int32_t& y0, int32_t& x1,
                          int32_t& y1) {
  const uint32_t tipo = reg.fetch_vertices[0] & 0x3;
  const uint32_t direccion = reg.fetch_vertices[0] >> 2;
  const auto orden = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
  const uint32_t tamano = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
  if (tipo != uint32_t(xenos::FetchConstantType::kVertex) || tamano != 3 * 2) {
    return Rechazar(5, "vertices de la copia en un formato no soportado");
  }
  const uint8_t* vertices = memoria_->TranslatePhysical(direccion * 4);
  const float medio_pixel = (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
  int32_t fijo[6];
  for (int i = 0; i < 6; ++i) {
    float valor;
    std::memcpy(&valor, vertices + i * 4, sizeof(valor));
    fijo[i] = Fijo16p8(xenos::GpuSwap(valor, orden) + medio_pixel);
  }
  x0 = (std::min({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
  y0 = (std::min({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;
  x1 = (std::max({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
  y1 = (std::max({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;
  const int32_t desplazamiento_x = ExtenderSigno15(reg.pa_sc_window_offset & 0x7FFF);
  const int32_t desplazamiento_y = ExtenderSigno15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
  if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
    x0 += desplazamiento_x;
    y0 += desplazamiento_y;
    x1 += desplazamiento_x;
    y1 += desplazamiento_y;
  }
  int32_t izquierda = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
  int32_t arriba = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
  int32_t derecha = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
  int32_t abajo = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
  if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
    izquierda += desplazamiento_x;
    arriba += desplazamiento_y;
    derecha += desplazamiento_x;
    abajo += desplazamiento_y;
  }
  izquierda = std::max(izquierda, 0);
  arriba = std::max(arriba, 0);
  derecha = std::max(derecha, izquierda);
  abajo = std::max(abajo, arriba);
  x0 = std::clamp(x0, izquierda, derecha);
  y0 = std::clamp(y0, arriba, abajo);
  x1 = std::clamp(x1, izquierda, derecha);
  y1 = std::clamp(y1, arriba, abajo);
  x0 &= ~int32_t(7);  // D3D9 aligns to 8 (kResolveAlignmentPixels)
  y0 &= ~int32_t(7);
  x1 = (x1 + 7) & ~int32_t(7);
  y1 = (y1 + 7) & ~int32_t(7);
  const int32_t pitch_alineado = int32_t(pitch & ~uint32_t(7));
  x0 = std::min(x0, pitch_alineado);
  x1 = std::min(x1, pitch_alineado);
  if (x0 >= x1 || y0 >= y1) {
    return Rechazar(6, "rectangulo de copia vacio");
  }
  return true;
}

// fc3-switch (PROTOTYPE, 2026-11-02): a resolve into part of a bigger texture (see the comment in Copiar). Finds the
// resolved texture of this pitch and height whose tiled bytes hold `base` at a whole 32x32-texel tile, and the tile's
// texel position in it.
bool RenderDk::BuscarMosaico(uint32_t base, uint32_t pitch_destino, uint32_t alto_destino, uint32_t ancho_rect,
                             uint32_t alto_rect, uint32_t& base_textura, uint32_t& dx, uint32_t& dy) {
  // A resolve of the whole texture size is never a part of another one.
  if (pitch_destino < 32 || !alto_destino || (ancho_rect + 32 > pitch_destino && alto_rect + 32 > alto_destino)) {
    return false;
  }
  const uint32_t tiles_fila = pitch_destino / 32;
  for (const auto& [base_t, t] : resueltas_) {
    if (base_t >= base || t.imagen.ancho != pitch_destino || t.imagen.alto != alto_destino) {
      continue;
    }
    const uint32_t bpp = t.imagen.profundidad ? 4u : BytesPorTexelResuelta(t.imagen.formato);
    const uint32_t desplazamiento = base - base_t;
    const uint32_t bytes_tile = 32u * 32u * bpp;
    if (desplazamiento % bytes_tile) {
      continue;
    }
    const uint32_t indice = desplazamiento / bytes_tile;
    const uint32_t tx = (indice % tiles_fila) * 32, ty = (indice / tiles_fila) * 32;
    // Inside that texture's tiled bytes, and the rectangle fits there. Run 29 without these bounds merged unrelated
    // textures that merely sat at higher addresses (13690000 taken as 13340000 + (256, 672)).
    if (indice >= tiles_fila * ((alto_destino + 31) / 32) || tx + ancho_rect > pitch_destino ||
        ty + alto_rect > alto_destino) {
      continue;
    }
    if (mosaicos_vistos_.insert(base).second) {
      REXLOG_INFO("[deko3d] resolve en mosaico: {:08X} es {:08X} + ({}, {})", base, base_t, tx, ty);
    }
    base_textura = base_t;
    dx = tx;
    dy = ty;
    return true;
  }
  return false;
}

// fc3-switch 2026-11-08 (EXPERIMENT, default off): fc3_dk_lecturas_sincronas: a resolve the game reads back on the
// CPU is written into guest memory before Copiar returns (the work is submitted and the GPU waited for), as on the
// console and in the PC build (readback_resolve = "full"), instead of at a later present when its slot's fence passed.
// Far Cry 3 computes its exposure on the CPU from such a readback (80x44 16_16_FLOAT); a few frames late it may read
// what was in memory before. Costs a GPU stall per readback.
bool RenderDk::Copiar(const RegistrosCopia& reg) {
  // fc3_dk_lecturas_ansiosas (HACK, 2026-11-09): finished read-backs reach guest memory at every resolve, not only at
  // the present (latency ~1 frame instead of 2-3, no stall).
  if (REXCVAR_GET(fc3_dk_lecturas_ansiosas)) {
    EscribirLecturasTerminadas();
  }
  lectura_sincrona_pendiente_ = false;
  const bool ok = CopiarInterno(reg);
  if (lectura_sincrona_pendiente_) {
    lectura_sincrona_pendiente_ = false;
    const uint64_t t0 = deko::AhoraNs();
    const uint64_t trabajo_antes = trabajo_;
    if (nucleo_.TrabajoAbierto()) {
      CerrarTrabajo();
    }
    const uint64_t t1 = deko::AhoraNs();
    // How far behind the GPU was (before the wait): the oldest unfinished work.
    uint32_t atraso = 0;
    for (uint64_t k = 1; k <= 2 && k <= trabajo_antes; ++k) {
      if (!nucleo_.TrabajoTerminado(trabajo_antes - k)) atraso = uint32_t(k);
    }
    proveedor_.dispositivo().EsperarInactiva();
    const uint64_t t2 = deko::AhoraNs();
    // fc3-switch 2026-11-09 DIAGNOSTIC: cost of the synchronous read-back (close + GPU wait), every 60th.
    static uint32_t n_sinc = 0;
    static uint64_t ns_cerrar = 0, ns_esperar = 0, atrasos = 0;
    ns_cerrar += t1 - t0;
    ns_esperar += t2 - t1;
    atrasos += atraso;
    if (++n_sinc % 60 == 0) {
      REXLOG_INFO("[lectsync] {} syncs: mean close {:.2f} ms, mean GPU wait {:.2f} ms, previous works unfinished {:.2f}",
                  n_sinc, double(ns_cerrar) / 60e6, double(ns_esperar) / 60e6, double(atrasos) / 60.0);
      ns_cerrar = ns_esperar = atrasos = 0;
    }
    EscribirLecturasTerminadas();  // every slot, oldest first: all of them are done now
    static uint32_t avisos = 0;
    if (avisos++ == 0) {
      REXLOG_INFO("[deko3d] fc3_dk_lecturas_sincronas: resolve read-backs written before the resolve returns");
    }
  }
  return ok;
}

bool RenderDk::CopiarInterno(const RegistrosCopia& reg) {
  AsegurarTrabajo();
  EmitirCieloPendiente();  // a resolve reads the pass's target: the deferred sky goes in first
  const bool hdr_previo = hdr_float_pendiente_ && hdr_float_dibujos_ == dibujados_;
  hdr_float_pendiente_ = false;
  const uint32_t control = reg.rb_copy_control;
  const uint32_t origen = control & 0x7;
  bool borrar_color = (control >> 8) & 0x1;
  const uint32_t comando = (control >> 20) & 0x3;
  const bool copiar =
      comando == uint32_t(xenos::CopyCommand::kRaw) || comando == uint32_t(xenos::CopyCommand::kConvert);
  const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
  if (origen >= xenos::kMaxColorRenderTargets) {
    // From depth (IsClearingColor: a depth source clears no color).
    if (copiar) {
      CopiarProfundidad(reg, pitch, (control >> 9) & 0x1);
    }
    if ((control >> 9) & 0x1) {
      BorrarProfundidad(reg, pitch);
    }
    return true;
  }
  const uint32_t info_color = reg.rb_color_info[origen];
  const uint32_t formato_color = (info_color >> 16) & 0xF;
  DkImageFormat formato_color_host;
  if (!FormatoDestinoColorDk(formato_color, formato_color_host)) {
    return Rechazar(100 + formato_color, "formato de destino de render todavia no soportado");
  }
  int32_t x0, y0, x1, y1;
  if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
    return false;
  }
  ImagenDk* destino_render = DestinoColor(info_color & 0xFFF, formato_color, pitch);
  if (!destino_render) {
    return false;
  }
  DkCmdBuf cmd = nucleo_.comandos();
  if (copiar) {
    const uint32_t info_destino = reg.rb_copy_dest_info;
    const uint32_t formato_destino = (info_destino >> 7) & 0x3F;
    Resuelta* resuelta = nullptr;
    uint32_t dx = 0, dy = 0;
    uint32_t base_resuelta = 0;
    // A copy into one slice of a 3D or array texture (copy_dest_array, the slice in bits 4-6): Dead Space 2 builds its
    // 32x32x32 color grading LUT that way every frame, 8 slices per destination base. Each slice is kept as a
    // resolved texture at base + slice * slice size, so the volume's slices have consecutive keys (texturas.cpp puts
    // them together as a 3D image). Only whole slices from the origin.
    const bool capa = (info_destino >> 3) & 0x1;
    if (capa && ((x0 & 31) || (y0 & 31))) {
      Rechazar(3, "copia a una capa de textura 3D o array desde un rectangulo desplazado: todavia no");
    } else if (DkImageFormat formato_copia; !FormatoResueltaDk(formato_destino, formato_copia)) {
      Rechazar(200 + formato_destino, "formato de copia todavia no soportado");
    } else {
      const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
      const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
      // The rectangle lands at its own position in the texture at the copy's base. The emulation (GetResolveInfo)
      // moves the base to the 32-texel tile that holds the rectangle, which in guest memory is the same bytes; here
      // textures are images keyed by base, so that made another texture the game never samples (Dead Space 2 resolves
      // a sub-rectangle of its 640x360 blur target in the boss fight: the real texture kept garbage).
      uint32_t base = reg.rb_copy_dest_base;
      if (capa) {
        base += ((info_destino >> 4) & 0x7) * pitch_destino * alto_destino * 4;
      }
      dx = uint32_t(x0);
      dy = uint32_t(y0);
      base_resuelta = base & 0x1FFFFFFF;
      // The copy destination gives the texture's pitch (a multiple of 32 texels) and its real height, not its width.
      // A resolve from the left edge that is narrower than the pitch by less than 32 texels is the whole texture and
      // the rest is padding: the texture is created at that width, or it is sampled across the padding too (with
      // nfsmw_resolucion_interna = escena_960x540 the 240-wide bloom lived in a 256-wide texture and the bloom,
      // shadows and reflections came out shifted to the left). At 1280x720 every resolved width is a multiple of 32.
      const uint32_t ancho_resolve = uint32_t(x1 - x0);
      const uint32_t ancho_textura =
          x0 == 0 && ancho_resolve < pitch_destino && ancho_resolve + 32 > pitch_destino ? ancho_resolve : pitch_destino;
      // fc3-switch (PROTOTYPE, 2026-11-02): resolves into part of a bigger texture. Far Cry 3 draws its 1280x704 scene
      // in two screen tiles (x 0-863, then 864-1279 with window offset -864) and its three 768x768 shadow cascades one
      // after another at the EDRAM origin, and resolves each to the base of the 32x32-texel tile where it belongs in
      // the texture (base + 27 tiles * 4 KB for screen x 864; + 24 and + 48 tiles for the cascades at x 768 and 1536).
      // Keyed by base, those made textures the game never samples: the right third of every scene buffer and two of
      // the three cascades stayed empty. When a resolved texture of this pitch and height holds the base inside its
      // tiled bytes, the copy lands there at the tile's position.
      const bool en_mosaico = !capa && BuscarMosaico(base_resuelta, pitch_destino, alto_destino, uint32_t(x1 - x0),
                                                     uint32_t(y1 - y0), base_resuelta, dx, dy);
      resuelta = ObtenerResuelta(base_resuelta, en_mosaico ? pitch_destino : ancho_textura, alto_destino,
                                 formato_destino, (info_destino >> 24) & 0x1, false);
    }
    if (resuelta) {
      const uint32_t pedido_ancho = std::min(uint32_t(x1 - x0), destino_render->ancho - uint32_t(x0));
      const uint32_t pedido_alto = std::min(uint32_t(y1 - y0), destino_render->alto - uint32_t(y0));
      const uint32_t cabe_ancho = resuelta->imagen.ancho > dx ? resuelta->imagen.ancho - dx : 0;
      const uint32_t cabe_alto = resuelta->imagen.alto > dy ? resuelta->imagen.alto - dy : 0;
      const uint32_t ancho = std::min(pedido_ancho, cabe_ancho);
      const uint32_t alto = std::min(pedido_alto, cabe_alto);
      // A scene larger than its resolved texture (an internal resolution above the front buffer's) is shrunk with
      // a linear blit, as the Vulkan path does; otherwise it is copied texel for texel.
      const bool encoger = cabe_ancho && cabe_alto &&
                           (pedido_ancho * 4 >= cabe_ancho * 5 || pedido_alto * 4 >= cabe_alto * 5);
      // nfsmw_nativo_frontal_perezoso: the whole scene to the front buffer of the last Swap, not read back.
      const int32_t maximo_lectura = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
      const bool se_lee = maximo_lectura > 0 && uint64_t(ancho) * alto <= uint64_t(maximo_lectura);
      // copy_dest_exp_bias (signed 6 bits at 16 of RB_COPY_DEST_INFO): the resolve scales by 2^bias. Gears of War 2
      // writes its scene x8 into the 7e3 EDRAM (c0 = 8 in every scene pixel shader) and resolves it with -3; ignored,
      // the post-processing got the scene 8 times too bright and burned it white. Only the 3D engine copy scales.
      const int32_t sesgo_exp = int32_t(reg.rb_copy_dest_info << 10) >> 26;
      const float escala_exp = std::ldexp(1.0f, sesgo_exp);
      if (sesgo_exp && sesgos_vistos_copia_.insert(sesgo_exp).second) {
        REXLOG_INFO("[deko3d] resolve con exp bias {} (x{}) desde EDRAM formato {}", sesgo_exp, escala_exp, formato_color);
      }
      // fc3-switch 2026-11-08 DIAGNOSTIC (HDR scale hunt): every biased resolve for a while: where, what, read back.
      if (sesgo_exp) {
        static uint32_t vistos_sesgo = 0;
        if (vistos_sesgo < 40 || (fotograma_ % 1000) == 0) {
          ++vistos_sesgo;
          REXLOG_INFO("[bias] frame {} resolve bias {} src EDRAM fmt {} pitch {} rect {},{} {}x{} -> base {:08X} dest fmt {} "
                      "{}x{} at {},{} read back {} info {:08X}",
                      fotograma_, sesgo_exp, formato_color, pitch, x0, y0, ancho, alto, base_resuelta, formato_destino,
                      resuelta->imagen.ancho, resuelta->imagen.alto, dx, dy, se_lee, reg.rb_copy_dest_info);
        }
      }
      const bool frontal_perezoso = REXCVAR_GET(nfsmw_nativo_frontal_perezoso);  // read each time: the game can switch it (ds2 safe mode)
      const bool frontal = frontal_perezoso && !frontal_apagado_ && sesgo_exp == 0 && base_resuelta == ultimo_frontal_ &&
                           !encoger &&
                           !se_lee && x0 == 0 && y0 == 0 && dx == 0 && dy == 0 && ancho == resuelta->imagen.ancho &&
                           alto == resuelta->imagen.alto;
      // What the resolve reads must be there: the useful area grows to it, a pending clear is done, the band too.
      if (y1 > 0) {
        destino_render->alto_usado =
            std::max(destino_render->alto_usado, std::min(uint32_t(y1), destino_render->alto));
      }
      HacerBorradoPendiente(*destino_render);
      CompletarBanda(*destino_render, uint32_t(std::max(y1, 0)), false, 0);
      if (frontal && uint32_t(x0) < destino_render->ancho && ancho && alto) {
        if (frontal_.pendiente && frontal_.base != base_resuelta) {
          MaterializarFrontal();
        }
        frontal_ = {true, base_resuelta, destino_render, resuelta, ancho, alto};
        frontal_obsoleta_ = 0;
        resuelta->escrita = true;
        ++resuelta->imagen.version;
        SombraMinimoResueltaEscrita(base_resuelta);
        ++frontales_ahorrados_;
      } else if (uint32_t(x0) < destino_render->ancho && uint32_t(y0) < destino_render->alto && ancho && alto) {
        const bool hdr = !encoger && !capa && !dx && !dy && !x0 && !y0 && ancho == 1280 && alto == 704 &&
                         formato_destino == 7 && !(info_color & 0xFFF) && pitch == 1280;
        const bool hdr_address = REXCVAR_GET(fc3_hdr_dynamic_addresses) || base_resuelta == 0x13384000;
        const bool reinterpretar = hdr && hdr_address && hdr_previo && (formato_color == 2 || formato_color == 10);
        ImagenDk* fuente = reinterpretar ? DestinoColor(0, 3, pitch) : destino_render;
        if (!fuente) return false;
        if (volcando_ && hdr && formato_color == 3 && REXCVAR_GET(fc3_trace_hdr_chain)) {
          // Snapshot this destination's preceding content just before the f3 resolve overwrites it.
          // Capturing every f12 would also capture unrelated intermediates and exhaust the readback block.
          VolcarResuelta(resuelta->imagen, 0xFC13380Cu, resuelta->formato_guest);
          REXLOG_INFO("[hdr-chain] pre-f3 frame={} address={:08X} image={} version={}", fotograma_,
                      base_resuelta, resuelta->imagen.serie, resuelta->imagen.version);
        }
        const DkImageView origen_vista = VistaCompleta(fuente->imagen);
        const DkImageView destino_vista = VistaCompleta(resuelta->imagen.imagen);
        // On the 3D engine, except the shrink (a linear blit) and the small resolves the game reads back (their
        // read-back is a copy engine transfer: both go there, one after the other, with no barrier between).
        tipo_copia_ = "resolve";
        if (encoger || (se_lee && sesgo_exp == 0 && !reinterpretar) ||
            !Copiar3D(*fuente, uint32_t(x0), uint32_t(y0), ancho, alto, resuelta->imagen, dx, dy, nullptr,
                      formato_color == 3 || formato_color == 12 || reinterpretar, escala_exp, false, false, reinterpretar)) {
          if (reinterpretar) return Rechazar(43, "HDR reinterpret copy failed");
          MarcarGpu(kGpuCopias);
          AntesDeTransferir();
          if (encoger) {
            const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, pedido_ancho, pedido_alto, 1};
            const DkImageRect b = {dx, dy, 0, cabe_ancho, cabe_alto, 1};
            dkCmdBufBlitImage(cmd, &origen_vista, &a, &destino_vista, &b, DkBlitFlag_FilterLinear, 0);
          } else {
            const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, ancho, alto, 1};
            const DkImageRect b = {dx, dy, 0, ancho, alto, 1};
            dkCmdBufCopyImage(cmd, &origen_vista, &a, &destino_vista, &b, 0);
          }
        }
        resuelta->escrita = true;
        ++resuelta->imagen.version;
        VolcarCopiaSiPedida(base_resuelta, resuelta->imagen, resuelta->formato_guest);
        SombraMinimoResueltaEscrita(base_resuelta);
        if (volcando_ && hdr && formato_color == 3 && (REXCVAR_GET(fc3_trace_lighting_checkpoint) || REXCVAR_GET(fc3_trace_hdr_chain))) {
          VolcarResuelta(*destino_render, 0xFC000003u, 3);
        }
        hdr_float_pendiente_ = hdr && hdr_address && !borrar_color && (formato_color == 3 || formato_color == 12);
        hdr_float_dibujos_ = dibujados_;
        ++copias_;
        if (volcando_ && hdr) {
          REXLOG_INFO("[fc3-hdr] frame={} address={:08X} f{} reinterpret={} image={} version={} dump={}",
                      fotograma_, base_resuelta, formato_color, reinterpretar,
                      resuelta->imagen.serie, resuelta->imagen.version, volcado_.size());
          if (!REXCVAR_GET(fc3_trace_hdr_chain) || formato_color != 12) {
            VolcarResuelta(resuelta->imagen,
                           REXCVAR_GET(fc3_trace_hdr_chain) ? (0xFC133800u | formato_color) : base_resuelta + formato_color,
                           resuelta->formato_guest);
          }
        } else if (volcando_) {
          VolcarResuelta(resuelta->imagen, base_resuelta, resuelta->formato_guest);
        }
        if (encoger) {
          LeerResuelta(reg, resuelta->imagen, x0, y0, dx, dy, cabe_ancho, cabe_alto);
        } else {
          LeerResuelta(reg, resuelta->imagen, x0, y0, dx, dy, ancho, alto);
        }
        OlvidarEstado();
      }
    }
  }
  // nfsmw_nativo_saltar_borrados_repetidos: the target already holds that value and nothing has been drawn since.
  const bool saltar_repetidos = REXCVAR_GET(nfsmw_nativo_saltar_borrados_repetidos);  // read each time: the game can switch it (ds2 safe mode)
  if (borrar_color && saltar_repetidos) {
    const uint64_t valor = uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
    const auto it = borrado_previo_.find(destino_render->serie);
    if (it != borrado_previo_.end() && it->second.dibujos == dibujados_ && it->second.valor == valor) {
      ++borrados_saltados_;
      borrar_color = false;
    }
  }
  if (borrar_color) {
    const uint64_t valor = uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
    borrado_previo_[destino_render->serie] = {valor, dibujados_};
    for (uint32_t j = 0; j < 4; ++j) {
      destino_render->borrado_color[j] = float((valor >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
    }
    // Done when the target is next bound (EmpezarPase), resolved or painted, not now: the front buffer may still be
    // painted from it, and a later clear makes this one unnecessary.
    destino_render->borrado_pendiente = true;
    ++borrados_;
    const bool aplazar = REXCVAR_GET(nfsmw_dk_borrado_color_aplazado);  // read each time: the game can switch it (ds2 safe mode)
    if (aplazar) {
      ++borrados_aplazados_;
      // The target may be bound in the current pass, and a draw with the same targets does not bind them again: the
      // pass ends here so the next draw goes through EmpezarPase, which does the clear. Without it the clear waited for
      // the resolve and wiped what had been drawn since (Dead Space 2's decompression scene: some particles bind the
      // G-buffer before its clear, then the whole G-buffer pass was cleared away at its resolve).
      g_.pase_valido = false;
    } else {
      HacerBorradoPendiente(*destino_render);
    }
  }
  if ((control >> 9) & 0x1) {
    BorrarProfundidad(reg, pitch);
  }
  return true;
}

void RenderDk::BorrarProfundidad(const RegistrosCopia& reg, uint32_t pitch) {
  const uint32_t info = reg.rb_depth_info;
  ImagenDk* profundidad = DestinoProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch);
  if (!profundidad) {
    return;
  }
  SombraMinimoAntesDeBorrar(*profundidad);
  profundidad->contenido_invalido = false;  // cleared: what a swap took away is no longer needed
  const bool saltar_repetidos = REXCVAR_GET(nfsmw_nativo_saltar_borrados_repetidos);  // read each time: the game can switch it (ds2 safe mode)
  EstadoBorrado& previo = borrado_previo_[profundidad->serie];
  if (saltar_repetidos && previo.dibujos == dibujados_ && previo.valor == reg.rb_depth_clear) {
    ++borrados_saltados_;
    return;
  }
  const bool solo_estencil = (reg.rb_copy_control & kCopiaSoloEstencil) != 0;
  previo = {solo_estencil ? ~0u : reg.rb_depth_clear, dibujados_};
  ++borrados_profundidad_;
  if (solo_estencil) {
    // A full clear still pending keeps its depth value and takes the new stencil; otherwise only the stencil goes.
    if (!profundidad->borrado_pendiente) profundidad->borrado_solo_estencil = true;
  } else {
    profundidad->borrado_z = float(reg.rb_depth_clear >> 8) / 16777215.0f;
    profundidad->borrado_solo_estencil = false;
  }
  profundidad->borrado_estencil = uint8_t(reg.rb_depth_clear & 0xFF);
  profundidad->borrado_pendiente = true;
  if (!solo_estencil && REXCVAR_GET(fc3_dk_alias_borrado) &&
      colores_8888_.count((uint64_t(info & 0xFFF) << 16) | pitch)) {
    if (ImagenDk* color = DestinoColor(info & 0xFFF, 0, pitch)) {
      // The guest's clear word is the D24S8 word: byte 0 stencil, bytes 1-3 depth, as the color view reads them.
      for (int k = 0; k < 4; ++k) color->borrado_color[k] = float((reg.rb_depth_clear >> (8 * k)) & 0xFF) / 255.0f;
      color->borrado_pendiente = true;
      static uint32_t avisos = 0;
      if (avisos++ < 4) {
        REXLOG_INFO("[deko3d] fc3_dk_alias_borrado: depth clear {:08X} at {:03X} (pitch {}) also clears its 8888 color",
                    reg.rb_depth_clear, info & 0xFFF, pitch);
      }
      if (!REXCVAR_GET(nfsmw_dk_borrado_color_aplazado)) HacerBorradoPendiente(*color);
    }
  }
  const bool aplazar = REXCVAR_GET(nfsmw_dk_borrado_z_aplazado);  // read each time: the game can switch it (ds2 safe mode)
  if (aplazar) {
    ++borrados_aplazados_;  // done when the target is next bound (EmpezarPase), copied or swapped
    g_.pase_valido = false;  // even if it is bound now (see Copiar)
    return;
  }
  HacerBorradoPendiente(*profundidad);
}

void RenderDk::HacerBorradoPendiente(ImagenDk& destino) {
  if (!destino.borrado_pendiente) {
    return;
  }
  AntesDeCambiarDestino(&destino);
  destino.borrado_pendiente = false;
  AsegurarTrabajo();
  EmitirCieloPendiente();
  AntesDe3D();
  MarcarGpu(kGpuBorrados);
  const DkImageView vista = VistaCompleta(destino.imagen);
  if (destino.profundidad) {
    dkCmdBufBindRenderTargets(nucleo_.comandos(), nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(nucleo_.comandos(), &vista, nullptr);
  }
  BorrarFilas(destino, 0, 0, AltoDeBorrado(destino));
  OlvidarEstado();
}

uint32_t RenderDk::AltoDeBorrado(const ImagenDk& destino) const {
  // fc3-switch (2026-11-09): depth targets are always cleared to full height. CopiarProfundidad resolves the
  // whole buffer into a texture that shaders sample; if rows below alto_usado keep stale Z data, the stale
  // depth produces rectangular artifacts on buildings (sharp 90-degree edges where the stale band meets the
  // cleared area). Color targets can still use alto_usado: they are not read beyond the visible region.
  if (destino.profundidad) {
    return destino.alto;
  }
  const bool area_util = REXCVAR_GET(nfsmw_dk_borrar_area_util);  // read each time: the game can switch it (ds2 safe mode)
  if (!area_util || !destino.alto_usado) {
    return destino.alto;
  }
  return std::min(destino.alto, (destino.alto_usado + 7) & ~7u);
}

void RenderDk::BorrarFilas(ImagenDk& destino, uint32_t i, uint32_t desde, uint32_t hasta) {
  if (hasta <= desde) {
    return;
  }
  DkCmdBuf cmd = nucleo_.comandos();
  const DkScissor tijera = {0, desde, destino.ancho, hasta - desde};
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);  // clears respect the scissor (HowToDeko3d.md, section 6, item 9)
  if (destino.profundidad) {
    // The clear also sets the ZCULL data of the bound depth (deko3d).
    dkCmdBufClearDepthStencil(cmd, !destino.borrado_solo_estencil, destino.borrado_z, 0xFF, destino.borrado_estencil);
  } else {
    const float* c = destino.borrado_color;
    dkCmdBufClearColorFloat(cmd, i, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
  }
  destino.alto_borrado = desde == 0 ? hasta : std::max(destino.alto_borrado, hasta);
  destino.banda_sucia = destino.alto_borrado < destino.alto;
}

void RenderDk::CompletarBanda(ImagenDk& destino, uint32_t hasta, bool ligado, uint32_t i) {
  hasta = std::min(hasta, destino.alto);
  if (hasta > destino.alto_usado) {
    destino.alto_usado = hasta;
  }
  // A pending clear will cover the useful area as it is when it is done.
  if (!destino.banda_sucia || hasta <= destino.alto_borrado || destino.borrado_pendiente) {
    return;
  }
  const uint32_t desde = destino.alto_borrado;
  const uint32_t hasta_banda = std::max(AltoDeBorrado(destino), std::min(destino.alto, (hasta + 7) & ~7u));
  ++bandas_completadas_;
  if (ligado) {
    BorrarFilas(destino, i, desde, hasta_banda);  // the target is bound in the current pass
    g_.valido = false;  // the scissor is the clear's
    return;
  }
  AntesDeCambiarDestino(&destino);
  AsegurarTrabajo();
  EmitirCieloPendiente();
  AntesDe3D();
  MarcarGpu(kGpuBorrados);
  const DkImageView vista = VistaCompleta(destino.imagen);
  if (destino.profundidad) {
    dkCmdBufBindRenderTargets(nucleo_.comandos(), nullptr, 0, &vista);
  } else {
    dkCmdBufBindRenderTarget(nucleo_.comandos(), &vista, nullptr);
  }
  BorrarFilas(destino, 0, desde, hasta_banda);
  OlvidarEstado();
}

void RenderDk::MaterializarFrontal() {
  if (!frontal_.pendiente) {
    return;
  }
  frontal_.pendiente = false;
  ++frontales_copiados_;
  ImagenDk& destino = frontal_.resuelta->imagen;
  tipo_copia_ = "frontal";
  if (!Copiar3D(*frontal_.destino, 0, 0, frontal_.ancho, frontal_.alto, destino, 0, 0)) {
    AsegurarTrabajo();
    MarcarGpu(kGpuCopias);
    AntesDeTransferir();
    const DkImageView origen_vista = VistaCompleta(frontal_.destino->imagen);
    const DkImageView destino_vista = VistaCompleta(destino.imagen);
    const DkImageRect rect = {0, 0, 0, frontal_.ancho, frontal_.alto, 1};
    dkCmdBufCopyImage(nucleo_.comandos(), &origen_vista, &rect, &destino_vista, &rect, 0);
  }
}

void RenderDk::IntercambiarImagenes(ImagenDk& destino, ImagenDk& resuelta) {
  std::swap(destino.imagen, resuelta.imagen);
  std::swap(destino.memoria, resuelta.memoria);
  std::swap(destino.serie, resuelta.serie);  // the descriptors go with their image
  ++generacion_texturas_;  // the address now samples another image: the per-fetch cache looks again
  borrado_previo_.erase(destino.serie);  // the target holds another image: its last clear no longer holds
  OlvidarEstado();  // the bound target, if it was this one, is another image now
  invalidar_zcull_ |= destino.profundidad;
  ++intercambios_;
}

void RenderDk::RestaurarContenido(ImagenDk& destino, bool para_resolver) {
  if (!destino.contenido_invalido) {
    return;
  }
  destino.contenido_invalido = false;
  const auto it = resueltas_.find(destino.resuelta_base);
  if (it == resueltas_.end() || it->second.imagen.ancho != destino.ancho || it->second.imagen.alto != destino.alto ||
      it->second.imagen.formato != destino.formato) {
    return;  // unknown where it went: the target keeps what it has, as the Vulkan path
  }
  // nfsmw_nativo_sombra_minimo: the car pass of the shadow map, drawn over a cleared target instead.
  if (!para_resolver && SombraMinimoEnVezDeRestaurar(destino)) {
    return;
  }
  AsegurarTrabajo();
  EmitirCieloPendiente();
  // fc3_dk_copiar_alto_usado: only the rows the game uses (alto_usado, when known).
  const uint32_t alto_copia = REXCVAR_GET(fc3_dk_copiar_alto_usado) && destino.alto_usado
                                  ? std::min(destino.alto, destino.alto_usado)
                                  : destino.alto;
  tipo_copia_ = "restaurar";
  if (!Copiar3D(it->second.imagen, 0, 0, destino.ancho, alto_copia, destino, 0, 0)) {
    MarcarGpu(kGpuCopias);
    AntesDeTransferir();
    const DkImageView origen_vista = VistaCompleta(it->second.imagen.imagen);
    const DkImageView destino_vista = VistaCompleta(destino.imagen);
    const DkImageRect rect = {0, 0, 0, destino.ancho, alto_copia, 1};
    dkCmdBufCopyImage(nucleo_.comandos(), &origen_vista, &rect, &destino_vista, &rect, 0);
  }
  borrado_previo_.erase(destino.serie);
  invalidar_zcull_ |= destino.profundidad;
  destino.alto_borrado = alto_copia;  // only the copied rows hold valid content: the rest may be stale
  destino.banda_sucia = alto_copia < destino.alto;
  ++restauraciones_;
  OlvidarEstado();
}

// Depth to a resolved texture of the host depth format, which the draws sample as k_24_8: the Vulkan path's
// CopiarProfundidad without the scaled shadow map. A whole target resolved to a texture of its size and format (the
// 1600x1600 shadow maps) swaps images instead of copying (nfsmw_nativo_resolver_sin_copia), with or without the clear
// (nfsmw_nativo_intercambiar_sin_borrado); the target gets its content back if the game draws on it again first.
void RenderDk::CopiarProfundidad(const RegistrosCopia& reg, uint32_t pitch, bool borrara) {
  const uint32_t info_destino = reg.rb_copy_dest_info;
  if ((info_destino >> 3) & 0x1) {
    Rechazar(3, "copia a textura 3D o array: todavia no");
    return;
  }
  int32_t x0, y0, x1, y1;
  if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
    return;
  }
  const uint32_t info = reg.rb_depth_info;
  ++copias_profundidad_;
  ImagenDk* profundidad = DestinoProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch);
  if (!profundidad) {
    return;
  }
  const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
  const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
  // At its own position in the texture at the copy's base, as for color (Copiar).
  uint32_t base = reg.rb_copy_dest_base & 0x1FFFFFFF;
  uint32_t dx = uint32_t(x0);
  uint32_t dy = uint32_t(y0);
  // fc3-switch: Far Cry 3's three shadow cascades go to base + 0/24/48 tiles of one 2304x768 texture (BuscarMosaico).
  const bool en_mosaico = BuscarMosaico(base, pitch_destino, alto_destino, uint32_t(x1 - x0), uint32_t(y1 - y0), base,
                                        dx, dy);
  const uint32_t formato_textura = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
  // Its width as for color (the pitch is padded to 32 texels; see Copiar).
  const uint32_t ancho_resolve = uint32_t(x1 - x0);
  const uint32_t ancho_textura =
      x0 == 0 && ancho_resolve < pitch_destino && ancho_resolve + 32 > pitch_destino ? ancho_resolve : pitch_destino;
  Resuelta* resuelta =
      ObtenerResuelta(base, en_mosaico ? pitch_destino : ancho_textura, alto_destino, formato_textura, false, true);
  if (!resuelta) {
    return;
  }
  const uint32_t ancho = std::min({uint32_t(x1 - x0), profundidad->ancho - uint32_t(x0), resuelta->imagen.ancho - dx});
  const uint32_t alto = std::min({uint32_t(y1 - y0), profundidad->alto - uint32_t(y0), resuelta->imagen.alto - dy});
  if (!ancho || !alto || uint32_t(x0) >= profundidad->ancho || uint32_t(y0) >= profundidad->alto ||
      dx >= resuelta->imagen.ancho || dy >= resuelta->imagen.alto) {
    return;
  }
  AsegurarTrabajo();
  EmitirCieloPendiente();
  // What is resolved must be the target's real content: a swap may have taken it away, a clear may be pending, the
  // useful area grows to what is read.
  RestaurarContenido(*profundidad, true);
  if (y1 > 0) {
    profundidad->alto_usado = std::max(profundidad->alto_usado, std::min(uint32_t(y1), profundidad->alto));
  }
  HacerBorradoPendiente(*profundidad);
  CompletarBanda(*profundidad, uint32_t(std::max(y1, 0)), false, 0);
  // fc3-switch (PROTOTYPE): this base was fetched as k_8_8_8_8 (texturas.cpp): its bytes with the stencil as well,
  // read from the target before the copy or swap below takes its content.
  if (auto zc = z_como_color_.find(base); zc != z_como_color_.end()) {
    ZComoColor& c = zc->second;
    if (c.imagen.viva && (c.imagen.ancho != resuelta->imagen.ancho || c.imagen.alto != resuelta->imagen.alto)) {
      DestruirImagen(c.imagen);
      c.escrita = false;
    }
    if (!c.imagen.viva) {
      if (CrearImagen(c.imagen, DkImageType_2D, DkImageFormat_RGBA8_Unorm, resuelta->imagen.ancho,
                      resuelta->imagen.alto, 1, 1, DkImageFlags_UsageRender, "profundidad como color")) {
        std::fill(std::begin(c.imagen.borrado_color), std::end(c.imagen.borrado_color), 0.0f);
        c.imagen.borrado_pendiente = true;
        HacerBorradoPendiente(c.imagen);
        REXLOG_INFO("[deko3d] profundidad resuelta en {:08X} ({}x{}, formato {}) leida como color: copia con estencil",
                    base, c.imagen.ancho, c.imagen.alto, formato_textura);
      }
    }
    const int32_t modos[4] = {formato_textura == 23 ? 1 : 0, 0, 0, 0};
    if (c.imagen.viva &&
        Copiar3D(*profundidad, uint32_t(x0), uint32_t(y0), ancho, alto, c.imagen, dx, dy, modos, false, 1.0f, true)) {
      c.escrita = true;
      ++c.imagen.version;
      if (volcando_) {
        VolcarResuelta(c.imagen, base | 1, 6);  // DIAGNOSTIC: shows as <base+1>_..._g6 (R = stencil)
      }
    }
  }
  const bool sin_copia = REXCVAR_GET(nfsmw_nativo_resolver_sin_copia);  // read each time: the game can switch it (ds2 safe mode)
  const bool entero = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 && ancho == profundidad->ancho &&
                      alto == profundidad->alto && resuelta->imagen.ancho == profundidad->ancho &&
                      resuelta->imagen.alto == profundidad->alto && resuelta->imagen.formato == profundidad->formato;
  if (sin_copia && entero && (borrara || REXCVAR_GET(nfsmw_nativo_intercambiar_sin_borrado))) {
    IntercambiarImagenes(*profundidad, resuelta->imagen);
    profundidad->contenido_invalido = true;  // its content is in the texture now
    profundidad->resuelta_base = base;
    resuelta->escrita = true;
    ++resuelta->imagen.version;
    VolcarCopiaSiPedida(base, resuelta->imagen, resuelta->formato_guest);
    SombraMinimoResueltaEscrita(base);
    SombraMinimoTrasIntercambio(*profundidad, base, borrara);
    return;
  }
  SombraMinimoCopiaDesde(*profundidad);
  // On the 3D engine (gl_FragDepth): compressed depth must not go through the copy engine.
  tipo_copia_ = "resolve z";
  if (!Copiar3D(*profundidad, uint32_t(x0), uint32_t(y0), ancho, alto, resuelta->imagen, dx, dy)) {
    MarcarGpu(kGpuCopias);
    AntesDeTransferir();
    const DkImageView origen_vista = VistaCompleta(profundidad->imagen);
    const DkImageView destino_vista = VistaCompleta(resuelta->imagen.imagen);
    const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, ancho, alto, 1};
    const DkImageRect b = {dx, dy, 0, ancho, alto, 1};
    dkCmdBufCopyImage(nucleo_.comandos(), &origen_vista, &a, &destino_vista, &b, 0);
  }
  resuelta->escrita = true;
  ++resuelta->imagen.version;
  VolcarCopiaSiPedida(base, resuelta->imagen, resuelta->formato_guest);
  SombraMinimoResueltaEscrita(base);
  ++copias_;
  if (volcando_) {
    VolcarResuelta(resuelta->imagen, base, resuelta->formato_guest);
  }
  OlvidarEstado();
}

// ---- nfsmw_nativo_sombra_minimo (the Vulkan backend's, nfsmw_nativo_destinos.cpp, with the same guards)

void RenderDk::SombraMinimoDiferencia(const char* motivo, uint32_t dato) {
  ++sm_diferencias_;
  if (sm_fase_ != kSmApagada) {
    sm_fase_ = kSmApagada;
    REXLOG_ERROR("[deko3d] sombra por minimo: DIFERENCIA ({}; dato {:08X}). Se apaga para la sesion: el mapa de "
                 "sombras se vuelve a restaurar copiando",
                 motivo, dato);
  }
}

void RenderDk::SombraMinimoNoApto(const char* motivo, uint32_t dato) {
  if (sm_fase_ != kSmApagada) {
    sm_fase_ = kSmApagada;
    REXLOG_WARN("[deko3d] sombra por minimo: no se aplica en esta sesion ({}; dato {:08X})", motivo, dato);
  }
}

void RenderDk::SombraMinimoFalloCiclo(const char* motivo, uint32_t dato, bool permanente) {
  sm_ciclo_limpio_ = false;
  if (sm_ciclo_borrado_) {
    SombraMinimoDiferencia(motivo, dato);
  } else if (permanente) {
    SombraMinimoNoApto(motivo, dato);
  }
}

bool RenderDk::SombraMinimoEncendido() const {
  return REXCVAR_GET(nfsmw_nativo_sombra_minimo) && REXCVAR_GET(nfsmw_nativo_pcf_barato);
}

// After a whole depth target was resolved by swapping: without a clear it starts a cycle (the texture got the world),
// with a clear after the car pass it ends it (the texture got the cars).
void RenderDk::SombraMinimoTrasIntercambio(const ImagenDk& destino, uint32_t base, bool borrara) {
  if (destino.ancho != kLadoSombras || destino.alto != kLadoSombras) {
    return;
  }
  if (!borrara) {
    if (sm_estado_ == kSmCoches && sm_ciclo_borrado_) {
      SombraMinimoDiferencia("el destino con los coches solos se resuelve sin borrar", base);
    }
    if (sm_estado_ != kSmLibre) {
      sm_limpios_seguidos_ = 0;
    }
    sm_estado_ = kSmTrasMundo;
    sm_destino_ciclo_ = &destino;
    sm_mundo_ciclo_ = base;
    sm_ciclo_limpio_ = true;
    sm_ciclo_borrado_ = false;
    sm_mundo_reescrito_ = false;
    sm_encendido_ = SombraMinimoEncendido();
    return;
  }
  if (sm_estado_ != kSmCoches || &destino != sm_destino_ciclo_) {
    if (sm_estado_ != kSmLibre) {
      sm_limpios_seguidos_ = 0;
    }
    sm_estado_ = kSmLibre;
    return;
  }
  sm_estado_ = kSmLibre;
  ++sm_ciclos_;
  if (sm_ciclo_borrado_) {
    if (base != sm_coches_) {
      SombraMinimoDiferencia("la resolucion de los coches va a otra textura", base);
      sm_coches_ = base;
    }
    sm_virtual_ = true;
    sm_virtual_valida_ = !sm_mundo_reescrito_;
    if (sm_mundo_reescrito_) {
      SombraMinimoDiferencia("la textura del mundo se volvio a escribir dentro del ciclo", sm_mundo_);
    }
    ++sm_ciclos_aplicados_;
    return;
  }
  if (base != sm_coches_ || sm_mundo_ciclo_ != sm_mundo_ || sm_destino_ciclo_ != sm_destino_) {
    if (sm_fase_ == kSmAplicando) {
      SombraMinimoNoApto("aparece un ciclo de mapa de sombras distinto del aprendido (el dato es su textura)", base);
      return;
    }
    sm_coches_ = base;
    sm_mundo_ = sm_mundo_ciclo_;
    sm_destino_ = sm_destino_ciclo_;
    sm_limpios_seguidos_ = 0;
  }
  sm_limpios_seguidos_ = (sm_ciclo_limpio_ && !sm_mundo_reescrito_) ? sm_limpios_seguidos_ + 1 : 0;
  if (sm_fase_ == kSmMirando && sm_encendido_ && sm_limpios_seguidos_ >= kSombraMinimoCiclos) {
    sm_fase_ = kSmAplicando;
    REXLOG_INFO("[deko3d] sombra por minimo: APLICANDO tras {} ciclos limpios seguidos (mundo en {:08X}, coches en "
                "{:08X}): el pase de los coches se dibuja sobre el destino borrado y no se copia",
                sm_limpios_seguidos_, sm_mundo_, sm_coches_);
  }
}

// From RestaurarContenido: the target lost its content in a swap and is about to be drawn on. While applying, the car
// pass gets a clear to 1.0 (done when the pass binds it) instead of the world copied back.
bool RenderDk::SombraMinimoEnVezDeRestaurar(ImagenDk& destino) {
  if (&destino != sm_destino_ciclo_ || sm_estado_ != kSmTrasMundo) {
    if (&destino == sm_destino_ciclo_ && sm_estado_ == kSmCoches) {
      SombraMinimoFalloCiclo("el destino de los coches se restaura otra vez", destino.resuelta_base, false);
    }
    return false;
  }
  if (destino.resuelta_base != sm_mundo_ciclo_) {
    SombraMinimoFalloCiclo("el destino se restaura desde otra textura", destino.resuelta_base, false);
    return false;
  }
  sm_estado_ = kSmCoches;
  if (sm_fase_ != kSmAplicando || !sm_encendido_ || &destino != sm_destino_ || sm_mundo_ciclo_ != sm_mundo_ ||
      !sm_coches_) {
    return false;
  }
  destino.borrado_z = 1.0f;
  destino.borrado_estencil = 0;
  destino.borrado_solo_estencil = false;
  destino.borrado_pendiente = true;  // EmpezarPase clears it right after binding it
  borrado_previo_.erase(destino.serie);  // not the game's clear: its next one cannot be skipped
  sm_ciclo_borrado_ = true;
  return true;
}

void RenderDk::SombraMinimoCopiaDesde(const ImagenDk& origen) {
  if (&origen == sm_destino_ciclo_ && sm_estado_ != kSmLibre) {
    SombraMinimoFalloCiclo("se copia del destino del mapa de sombras a mitad del ciclo", 0, false);
  }
}

void RenderDk::SombraMinimoAntesDeBorrar(const ImagenDk& destino) {
  if (&destino == sm_destino_ciclo_ && sm_estado_ != kSmLibre) {
    sm_limpios_seguidos_ = 0;
    sm_estado_ = kSmLibre;
  }
}

void RenderDk::SombraMinimoResueltaEscrita(uint32_t base) {
  if (sm_virtual_) {
    if (base == sm_coches_) {
      sm_virtual_ = false;
    } else if (base == sm_mundo_) {
      sm_virtual_valida_ = false;
    }
  }
  if (sm_estado_ != kSmLibre && base == sm_mundo_ciclo_) {
    sm_mundo_reescrito_ = true;
  }
}

bool RenderDk::PaseDeCochesSombra(const ImagenDk* profundidad, bool solo_profundidad) {
  if (!profundidad || profundidad != sm_destino_ciclo_ || sm_estado_ != kSmCoches) {
    return false;
  }
  if (!solo_profundidad) {
    SombraMinimoFalloCiclo("el pase de los coches tiene destino de color", 0, true);
  }
  return true;
}

void RenderDk::DibujoDeCochesSombra(bool exacto, uint32_t control_z) {
  ++sm_dibujos_coches_;
  if (!exacto) {
    ++sm_dibujos_no_exactos_;
    SombraMinimoFalloCiclo("un dibujo de los coches no deja el minimo (el dato es su control de Z)", control_z, true);
  }
}

uint32_t RenderDk::DireccionSombraCoches() const {
  return (sm_virtual_ || (sm_encendido_ && sm_fase_ != kSmApagada)) ? sm_coches_ : 0;
}

const ImagenDk* RenderDk::CompaneraSombraCoches(bool capaz, uint32_t ps) {
  if (sm_virtual_) {
    const auto it = resueltas_.find(sm_mundo_);
    const bool mundo = it != resueltas_.end() && it->second.escrita;
    if (!sm_virtual_valida_ || !capaz || !mundo) {
      ++sm_lecturas_incapaces_;
      SombraMinimoDiferencia(!sm_virtual_valida_ ? "se muestrea la textura de los coches solos y la del mundo ya es de "
                                                   "otro ciclo (el dato es el pixel shader)"
                             : !capaz ? "un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches solos "
                                        "(el dato es su numero)"
                                      : "no esta la textura del mundo (el dato es el pixel shader)",
                             ps);
      return nullptr;
    }
    ++sm_lecturas_minimo_;
    return &it->second.imagen;
  }
  if (sm_estado_ != kSmLibre) {
    ++sm_lecturas_a_destiempo_;
    SombraMinimoNoApto("la textura de los coches se muestrea entre las dos resoluciones (el dato es el pixel shader)",
                       ps);
  }
  if (!capaz) {
    ++sm_lecturas_incapaces_;
    SombraMinimoNoApto("un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches (el dato es su numero)",
                       ps);
    return nullptr;
  }
  const auto it = resueltas_.find(sm_coches_);
  if (!sm_encendido_ || sm_fase_ == kSmApagada || it == resueltas_.end() || !it->second.escrita) {
    ++sm_lecturas_normales_;
    return nullptr;
  }
  ++sm_lecturas_si_misma_;  // the minimum with itself: the variants with the bit already exist when applying starts
  return &it->second.imagen;
}

// ---- Read-back of small resolved textures (the game's exposure)

void RenderDk::LeerResuelta(const RegistrosCopia& reg, const ImagenDk& imagen, int32_t x0, int32_t y0, uint32_t dx,
                            uint32_t dy, uint32_t ancho, uint32_t alto) {
  const int32_t maximo = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
  // Any 4-byte texel: written back as whole words (GpuSwap), the R/B swap only for 8_8_8_8 (EscribirLecturas). Dead
  // Space 2's exposure reads a 64x8 32_FLOAT resolve; without it the game computed its exposure from stale memory and
  // dark scenes came out white.
  if (maximo <= 0 || uint64_t(ancho) * alto > uint64_t(maximo) ||
      (imagen.formato != DkImageFormat_RGBA8_Unorm && imagen.formato != DkImageFormat_R32_Float &&
       imagen.formato != DkImageFormat_RGB10A2_Unorm && imagen.formato != DkImageFormat_RG16_Float)) {
    return;
  }
  LecturasRanura& l = lecturas_[trabajo_ % kRanuras];
  const uint32_t bytes = ancho * alto * 4;
  if (l.usados + bytes > kBytesLecturaRanura) {
    if (lecturas_sin_sitio_++ == 0) {
      REXLOG_WARN("[deko3d] lecturas de vuelta: no caben mas en la ranura ({} KB); el juego vera la anterior",
                  kBytesLecturaRanura >> 10);
    }
    return;
  }
  const uint32_t offset = uint32_t(trabajo_ % kRanuras) * kBytesLecturaRanura + l.usados;
  l.usados += (bytes + 255) & ~255u;
  dkMemBlockFlushCpuCache(lectura_bloque_, offset, bytes);  // nothing of it stays in the CPU's cache
  const DkImageView vista = VistaCompleta(imagen.imagen);
  const DkImageRect rect = {dx, dy, 0, ancho, alto, 1};
  const DkCopyBuf destino = {lectura_gpu_ + offset, ancho * 4, ancho * 4 * alto};
  // A transfer like the resolve: the copy engine runs them in order, and CerrarTrabajo waits for it before the fence.
  AntesDeTransferir();
  dkCmdBufCopyImageToBuffer(nucleo_.comandos(), &vista, &rect, &destino, 0);
  l.pendientes.push_back({offset, reg.rb_copy_dest_base, x0, y0, ancho, alto, reg.rb_copy_dest_pitch & 0x3FFF,
                          (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info});
  // fc3_dk_lecturas_sincronas_minmax (HACK, 2026-11-09): only the 80x44 min/max depth the CPU culls lights with.
  if (REXCVAR_GET(fc3_dk_lecturas_sincronas) ||
      (REXCVAR_GET(fc3_dk_lecturas_sincronas_minmax) && ancho == 80 && alto == 44)) {
    lectura_sincrona_pendiente_ = true;
  }
  if (lecturas_hechas_++ == 0) {
    REXLOG_INFO("[deko3d] lectura de vuelta de texturas resueltas de hasta {} texels (la primera: {:08X}, {}x{})",
                maximo, reg.rb_copy_dest_base, ancho, alto);
  }
}

// Loaded with its fetch constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the
// resolved texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it. As the Vulkan path.
void RenderDk::EscribirLecturas(uint32_t ranura) {
  LecturasRanura& l = lecturas_[ranura];
  if (l.pendientes.empty()) {
    l.usados = 0;
    return;
  }
  uint8_t* const fisica = memoria_->TranslatePhysical(0);
  const auto& tabla = TablaMosaico2DTexel4();
  for (const LecturaDk& p : l.pendientes) {
    dkMemBlockFlushCpuCache(lectura_bloque_, p.offset, p.ancho * p.alto * 4);  // what the GPU wrote, not the cache
    const uint8_t* datos = lectura_cpu_ + p.offset;
    // copy_dest_swap exchanges red and blue of 8-bit channels; a 32-bit float (or 10-bit) texel is one word.
    const uint32_t formato_copia = (p.info >> 7) & 0x3F;
    // fc3_dk_minmax_dilatar (HACK, 2026-11-09): the 80x44 min/max depth (16_16_FLOAT, R = min, G = max) the CPU
    // culls lights per 16x16 tile with reaches it 1-3 frames late; each tile takes the min of R and the max of G of
    // its neighbours within N tiles, so the tiles at the edges of moving characters keep a conservative range.
    std::vector<uint32_t> dilatada;
    if (const int32_t radio = REXCVAR_GET(fc3_dk_minmax_dilatar);
        radio > 0 && formato_copia == 31 && p.ancho == 80 && p.alto == 44) {
      const int32_t w = int32_t(p.ancho), h = int32_t(p.alto);
      std::vector<uint32_t> fuente(size_t(w) * h);
      std::memcpy(fuente.data(), datos, fuente.size() * 4);
      dilatada.resize(fuente.size());
      for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
          float minimo = MedioAFloat(uint16_t(fuente[size_t(y) * w + x]));
          float maximo = MedioAFloat(uint16_t(fuente[size_t(y) * w + x] >> 16));
          uint32_t r = fuente[size_t(y) * w + x] & 0xFFFF, g = fuente[size_t(y) * w + x] >> 16;
          for (int32_t dy = -radio; dy <= radio; ++dy) {
            for (int32_t dx = -radio; dx <= radio; ++dx) {
              const int32_t xx = x + dx, yy = y + dy;
              if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
              const uint32_t v = fuente[size_t(yy) * w + xx];
              const float a = MedioAFloat(uint16_t(v)), b = MedioAFloat(uint16_t(v >> 16));
              if (a < minimo) { minimo = a; r = v & 0xFFFF; }
              if (b > maximo) { maximo = b; g = v >> 16; }
            }
          }
          dilatada[size_t(y) * w + x] = r | (g << 16);
        }
      }
      datos = reinterpret_cast<const uint8_t*>(dilatada.data());
    }
    const bool ocho_bits = formato_copia == 6 || formato_copia == 8 || formato_copia == 50 || formato_copia == 4;
    const bool intercambio = ocho_bits && ((p.info >> 24) & 0x1);
    const uint32_t orden_copia = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
    const auto orden = orden_copia <= 3 ? static_cast<xenos::Endian>(orden_copia) : xenos::Endian::k8in32;
    // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap), computed once with marked bytes.
    const uint32_t permutacion = xenos::GpuSwap(intercambio ? uint32_t(0x03000102) : uint32_t(0x03020100), orden);
    const uint8_t b0 = uint8_t(permutacion), b1 = uint8_t(permutacion >> 8), b2 = uint8_t(permutacion >> 16),
                  b3 = uint8_t(permutacion >> 24);
    const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
    const uint64_t baldosas_por_fila = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
    for (uint32_t j = 0; j < p.alto; ++j) {
      const uint32_t ty = uint32_t(p.y0) + j;
      if (p.alto_destino && ty >= p.alto_destino) {
        break;
      }
      const uint64_t fila = base + ((uint64_t(ty >> 5) * baldosas_por_fila) << 12);
      const uint16_t* const local = tabla.data() + size_t(ty & 31) * 32;
      const uint8_t* s = datos + size_t(j) * p.ancho * 4;
      for (uint32_t i = 0; i < p.ancho; ++i, s += 4) {
        const uint32_t tx = uint32_t(p.x0) + i;
        if (tx >= p.pitch) {
          break;
        }
        const uint64_t direccion = fila + (uint64_t(tx >> 5) << 12) + local[tx & 31];
        if (direccion + 4 > 0x20000000) {
          continue;
        }
        const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
        uint8_t* const d = fisica + direccion;
        d[0] = t0;
        d[1] = t1;
        d[2] = t2;
        d[3] = t3;
      }
    }
    // fc3-switch 2026-11-12 DIAGNOSTIC: the 80x44 min/max depth (light culling) as the GPU gave it and as the guest
    // CPU will read it (big-endian 16-bit halves at the written address), for tiles inside/outside the house block.
    if (formato_copia == 31 && p.ancho == 80 && p.alto == 44) {
      static uint32_t veces_mm = 0;
      if ((veces_mm++ % 300) == 0) {
        static const uint32_t tiles[6][2] = {{30, 20}, {40, 22}, {48, 28}, {10, 20}, {70, 20}, {40, 38}};
        std::string t;
        for (const auto& c : tiles) {
          const uint32_t tx = uint32_t(p.x0) + c[0], ty = uint32_t(p.y0) + c[1];
          uint32_t gpu = 0;
          std::memcpy(&gpu, datos + (size_t(c[1]) * p.ancho + c[0]) * 4, 4);
          const uint64_t fila = base + ((uint64_t(ty >> 5) * baldosas_por_fila) << 12);
          const uint64_t dir = fila + (uint64_t(tx >> 5) << 12) + tabla[size_t(ty & 31) * 32 + (tx & 31)];
          const uint8_t* g = fisica + dir;
          const uint16_t h0 = uint16_t((g[0] << 8) | g[1]), h1 = uint16_t((g[2] << 8) | g[3]);
          t += fmt::format(" [{},{}] gpu {:.4f}/{:.4f} guest@{:08X} {:02X}{:02X}{:02X}{:02X} be16 {:.4f}/{:.4f}", c[0], c[1],
                           MedioAFloat(uint16_t(gpu)), MedioAFloat(uint16_t(gpu >> 16)), uint32_t(dir), g[0], g[1], g[2],
                           g[3], MedioAFloat(h0), MedioAFloat(h1));
        }
        REXLOG_INFO("[minmax] base {:08X} pitch {} info {:08X} x0 {} y0 {} orden {}{}", uint32_t(p.base), p.pitch, p.info,
                    p.x0, p.y0, orden_copia, t);
      }
    }
    // fc3-switch 2026-11-07 DIAGNOSTIC (exposure hunt): what the game's CPU gets from a 16_16_FLOAT readback (the
    // luminance it computes its exposure from): copy_dest_info fields, the host image's mean and the mean of the
    // halves as the guest reads them (big-endian 16-bit words at the written addresses, first texel words).
    if (formato_copia == 31) {
      static uint32_t informes = 0;
      if (informes < 12 || (fotograma_ % 1000) < 4) {
        double host_r = 0, host_g = 0, guest_a = 0, guest_b = 0;
        uint32_t n = 0;
        for (uint32_t j = 0; j < p.alto; ++j) {
          const uint32_t ty = uint32_t(p.y0) + j;
          const uint64_t fila = base + ((uint64_t(ty >> 5) * baldosas_por_fila) << 12);
          const uint16_t* const local = tabla.data() + size_t(ty & 31) * 32;
          for (uint32_t i = 0; i < p.ancho; ++i) {
            const uint32_t tx = uint32_t(p.x0) + i;
            uint32_t v;
            std::memcpy(&v, datos + (size_t(j) * p.ancho + i) * 4, 4);
            host_r += MedioAFloat(uint16_t(v));
            host_g += MedioAFloat(uint16_t(v >> 16));
            const uint8_t* g = fisica + fila + (uint64_t(tx >> 5) << 12) + local[tx & 31];
            guest_a += MedioAFloat(uint16_t((g[0] << 8) | g[1]));
            guest_b += MedioAFloat(uint16_t((g[2] << 8) | g[3]));
            ++n;
          }
        }
        uint32_t v0;
        std::memcpy(&v0, datos, 4);
        const uint32_t tx0 = uint32_t(p.x0), ty0 = uint32_t(p.y0);
        const uint8_t* g0 = fisica + base + ((uint64_t(ty0 >> 5) * baldosas_por_fila) << 12) +
                            (uint64_t(tx0 >> 5) << 12) + tabla[size_t(ty0 & 31) * 32 + (tx0 & 31)];
        REXLOG_INFO("[readback16] #{} frame {} work {} base {:08X} {}x{} at {},{} pitch {} height {} info {:08X} (endian {} fmt {} "
                    "number {} exp_bias {} swap {}): host mean R {:.5f} G {:.5f}; guest BE halves mean {:.5f} {:.5f}; "
                    "first host word {:08X} guest bytes {:02X}{:02X}{:02X}{:02X}",
                    informes, fotograma_, l.trabajo, p.base, p.ancho, p.alto, p.x0, p.y0, p.pitch, p.alto_destino, p.info, p.info & 7,
                    formato_copia, (p.info >> 13) & 7, int32_t(p.info << 10) >> 26, (p.info >> 24) & 1,
                    host_r / std::max(n, 1u), host_g / std::max(n, 1u), guest_a / std::max(n, 1u),
                    guest_b / std::max(n, 1u), v0, g0[0], g0[1], g0[2], g0[3]);
      }
      ++informes;
    }
    ++lecturas_escritas_;
  }
  l.pendientes.clear();
  l.usados = 0;
}

void RenderDk::EscribirLecturasTerminadas() {
  // Oldest work first: a later copy to the same address must be the one that stays.
  std::array<uint32_t, kRanuras> orden;
  for (uint32_t ranura = 0; ranura < kRanuras; ++ranura) {
    orden[ranura] = ranura;
  }
  std::sort(orden.begin(), orden.end(),
            [this](uint32_t a, uint32_t b) { return lecturas_[a].trabajo < lecturas_[b].trabajo; });
  for (const uint32_t ranura : orden) {
    LecturasRanura& l = lecturas_[ranura];
    if (l.pendientes.empty()) {
      continue;
    }
    // The open work's are not submitted yet: its slot's fence is still the previous use's.
    if ((nucleo_.TrabajoAbierto() && l.trabajo == trabajo_) || !nucleo_.TrabajoTerminado(l.trabajo)) {
      break;  // a newer one must not be written before this one
    }
    EscribirLecturas(ranura);
  }
}

// ---- Output

void RenderDk::RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& rampa) {
  rampa_juego_ = rampa;
  RecalcularRampa();
}

// The output table: the game's ramp followed by the menu's post-processing, exactly as the Vulkan path computes it
// (nfsmw_nativo_destinos.cpp, RecalcularRampa, where the order is explained).
void RenderDk::RecalcularRampa() {
  const rex::x360::PostProcess p =
      version_posproceso_ == UINT64_MAX ? rex::x360::PostProcess{} : hooks_.Posproceso();
  graduacion_ = p.activo && (p.saturacion != 1.0f || p.vibracion != 0.0f || p.vineta > 0.0f || p.lineas > 0.0f);
  const float tinte[3] = {p.tinte_r, p.tinte_g, p.tinte_b};
  const float inversa_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t c = 0; c < 3; ++c) {
      float x = float(rampa_juego_[i][c] & 0x3FF) / 1023.0f;
      if (p.activo) {
        x += c == 0 ? p.temperatura * 0.10f : c == 2 ? -p.temperatura * 0.10f : 0.0f;
        x += p.brillo;
        x = (x - 0.5f) * p.contraste + 0.5f;
        x = x + (x * tinte[c] - x) * p.tinte;
        if (!graduacion_) {
          x = std::clamp(std::pow(std::max(x, 0.0001f), inversa_gamma), 0.0f, 1.0f);
        }
      }
      rampa_valores_[i * 4 + c] = x;
    }
    rampa_valores_[i * 4 + 3] = 1.0f;
  }
  const size_t extra = 256 * 4;
  rampa_valores_[extra] = p.saturacion;
  rampa_valores_[extra + 1] = p.vibracion;
  rampa_valores_[extra + 2] = inversa_gamma;
  rampa_valores_[extra + 3] = 0.0f;
  rampa_valores_[extra + 4] = p.vineta;
  rampa_valores_[extra + 5] = p.lineas;
  rampa_valores_[extra + 6] = 0.0f;
  rampa_valores_[extra + 7] = 0.0f;
  rampa_subir_ = true;
}

bool RenderDk::CrearShadersSalida() {
  const deko::Compilador::Resultado vs = compilador_.Compilar(true, kVsSalida);
  vs_salida_valido_ = vs.ok && nucleo_.CargarShader(vs_salida_, vs.dksh, "salida (vertices)");
  if (!vs_salida_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar el vertex shader de la salida: {}", vs.log);
  }
  const deko::Compilador::Resultado color = compilador_.Compilar(false, kFsCopiaColor);
  fs_copia_color_valido_ = color.ok && nucleo_.CargarShader(fs_copia_color_, color.dksh, "copia de color");
  const deko::Compilador::Resultado signo = compilador_.Compilar(false, kFsCopiaSigno);
  fs_copia_signo_valido_ = signo.ok && nucleo_.CargarShader(fs_copia_signo_, signo.dksh, "copia con signo");
  if (!fs_copia_signo_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la copia con signo: {}", signo.log);
  }
  const deko::Compilador::Resultado c7e3 = compilador_.Compilar(false, kFsCopia7e3);
  fs_copia_7e3_valido_ = c7e3.ok && nucleo_.CargarShader(fs_copia_7e3_, c7e3.dksh, "copia de 7e3");
  if (!fs_copia_7e3_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la copia de 7e3: {}", c7e3.log);
  }
  const deko::Compilador::Resultado zc = compilador_.Compilar(false, kFsZComoColor);
  fs_z_como_color_valido_ = zc.ok && nucleo_.CargarShader(fs_z_como_color_, zc.dksh, "profundidad como color");
  if (!fs_z_como_color_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la copia de profundidad como color: {}", zc.log);
  }
  std::string color_a_z_glsl = kFsColorAZ;
  bool color_a_z_override = false;
  if (REXCVAR_GET(fc3_debug_shader_overrides)) {
    const auto path = rex::filesystem::GetExecutableFolder() / "debug-shaders" / "color_to_depth.frag";
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = input ? std::streamoff(input.tellg()) : -1;
    if (bytes > 0 && bytes <= 64 * 1024) {
      std::string source(size_t(bytes), '\0');
      input.seekg(0);
      if (input.read(source.data(), bytes)) {
        color_a_z_glsl = std::move(source);
        color_a_z_override = true;
        REXLOG_INFO("[fc3-shader-override] loading {} ({} bytes)", path.string(), bytes);
      }
    }
    if (!color_a_z_override) {
      REXLOG_WARN("[fc3-shader-override] cannot read {} (expected 1..65536 bytes); using embedded shader", path.string());
    }
  }
  deko::Compilador::Resultado caz = compilador_.Compilar(false, color_a_z_glsl);
  if (!caz.ok && color_a_z_override) {
    REXLOG_ERROR("[fc3-shader-override] compilation failed; using embedded shader: {}", caz.log);
    caz = compilador_.Compilar(false, kFsColorAZ);
  }
  fs_color_a_z_valido_ = caz.ok && nucleo_.CargarShader(fs_color_a_z_, caz.dksh, "color como profundidad");
  if (!fs_color_a_z_valido_) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la copia de color a profundidad y estencil: {}", caz.log);
  }
  const deko::Compilador::Resultado z = compilador_.Compilar(false, kFsCopiaProfundidad);
  fs_copia_profundidad_valido_ = z.ok && nucleo_.CargarShader(fs_copia_profundidad_, z.dksh, "copia de profundidad");
  if (!fs_copia_color_valido_ || !fs_copia_profundidad_valido_) {
    REXLOG_ERROR("[deko3d] no se pudieron compilar las copias en el motor 3D ({} / {}): se copia con el motor de copias",
                 color.log, z.log);
  }
  return vs_salida_valido_ && ShaderRampa(kRampaExacta) != nullptr;
}

const DkShader* RenderDk::ShaderRampa(uint32_t variante) {
  variante &= 7;
  if (rampas_[variante]) {
    return rampas_[variante].get();
  }
  if (rampas_intentadas_[variante]) {
    return nullptr;
  }
  rampas_intentadas_[variante] = true;
  std::string glsl = "#version 460\n";
  glsl += (variante & kRampaExacta) ? "#define EXACTA 1\n" : "#define EXACTA 0\n";
  glsl += (variante & kRampaGraduacion) ? "#define GRADUACION 1\n" : "#define GRADUACION 0\n";
  glsl += (variante & kRampaFxaa) ? "#define FXAA 1\n" : "#define FXAA 0\n";
  glsl += rampa_gamma_ ? "#define RAMPA 1\n" : "#define RAMPA 0\n";
  glsl += kFsRampa;
  const uint64_t inicio = deko::AhoraNs();
  const deko::Compilador::Resultado r = compilador_.Compilar(false, glsl);
  auto shader = std::make_unique<DkShader>();
  if (!r.ok || !nucleo_.CargarShader(*shader, r.dksh, "salida (rampa)")) {
    REXLOG_ERROR("[deko3d] no se pudo compilar la variante {} del pixel shader de la salida: {}", variante, r.log);
    return nullptr;
  }
  REXLOG_INFO("[deko3d] salida: variante {} (exacta {}, graduacion {}, FXAA {}) compilada en {:.1f} ms", variante,
              (variante & kRampaExacta) != 0, (variante & kRampaGraduacion) != 0, (variante & kRampaFxaa) != 0,
              double(deko::AhoraNs() - inicio) / 1e6);
  rampas_[variante] = std::move(shader);
  return rampas_[variante].get();
}

bool RenderDk::PintarSalida(DkImage& salida, uint32_t ancho_salida, uint32_t alto_salida, const ImagenDk& origen,
                            uint32_t ancho, uint32_t alto) {
  AsegurarTrabajo();
  EmitirCieloPendiente();
  if (volcando_ && REXCVAR_GET(fc3_trace_hdr_chain)) {
    VolcarResuelta(origen, 0xFC0000FDu, 6);
    REXLOG_INFO("[hdr-chain] output frame={} source={} version={} format={} size={}x{}", fotograma_,
                origen.serie, origen.version, uint32_t(origen.formato), origen.ancho, origen.alto);
  }
  AntesDe3D();
  MarcarGpu(kGpuOtros);  // the output pass
  // The menu's post-processing, if it changed (an atomic counter; the cvars are only read then).
  const uint64_t version = hooks_.VersionPosproceso();
  if (version != version_posproceso_) {
    version_posproceso_ = version;
    RecalcularRampa();
  }
  const bool exacta = ancho_salida == ancho && alto_salida == alto;
  const bool fxaa = hooks_.Fxaa();
  if (fxaa != fxaa_anotado_) {
    fxaa_anotado_ = fxaa;
    REXLOG_INFO("[deko3d] antialiasing en la salida: {}", fxaa ? "FXAA" : "ninguno");
  }
  const DkShader* fs = ShaderRampa((exacta ? kRampaExacta : 0) | (graduacion_ ? kRampaGraduacion : 0) |
                                   (fxaa ? kRampaFxaa : 0));
  if (!fs) {
    fs = ShaderRampa(kRampaExacta);
  }
  if (!fs || !vs_salida_valido_) {
    return false;
  }
  // The ramp's texture, created once and uploaded in the pre-list when it changes (the game's ramp, the menu).
  if (!rampa_tabla_.viva &&
      !CrearImagen(rampa_tabla_, DkImageType_2D, DkImageFormat_RGBA32_Float, 256, 1, 1, 1, 0, "tabla de la rampa")) {
    return false;
  }
  if (rampa_subir_) {
    const deko::Subida datos = Reservar(256 * 16, 256);
    if (!datos) {
      return false;
    }
    std::memcpy(datos.cpu, rampa_valores_.data(), 256 * 16);
    DkImageView vista_tabla;
    dkImageViewDefaults(&vista_tabla, &rampa_tabla_.imagen);
    const DkCopyBuf desde = {datos.gpu, 256 * 16, 256 * 16};
    const DkImageRect rect = {0, 0, 0, 256, 1, 1};
    dkCmdBufCopyBufferToImage(nucleo_.ComandosPrevios(), &desde, &vista_tabla, &rect, 0);
    rampa_subir_ = false;
  }
  const deko::Subida ubo = Reservar(kBytesUboRampa, DK_UNIFORM_BUF_ALIGNMENT);
  if (!ubo) {
    return false;
  }
  const int32_t desplazamiento[4] = {0, 0, 0, 0};
  const float inverso[4] = {1.0f / float(std::max(ancho, 1u)), 1.0f / float(std::max(alto, 1u)), 0.0f, 0.0f};
  std::memcpy(ubo.cpu, rampa_valores_.data() + 256 * 4, 8 * sizeof(float));  // mezcla, efectos
  std::memcpy(ubo.cpu + 32, desplazamiento, sizeof(desplazamiento));
  std::memcpy(ubo.cpu + 48, inverso, sizeof(inverso));
  const uint32_t ranura = VistaDe(origen, 0x688 /* RGBA */, 0x688);
  const uint32_t ranura_tabla = VistaDe(rampa_tabla_, 0x688, 0x688);
  DkCmdBuf cmd = nucleo_.comandos();
  // The presenter may still be sampling this image from its previous paint (rex/ui/deko3d/presentador.h).
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
  const DkImageView vista = VistaCompleta(salida);
  dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
  const DkViewport vp = {0.0f, 0.0f, float(ancho_salida), float(alto_salida), 0.0f, 1.0f};
  const DkScissor tijera = {0, 0, ancho_salida, alto_salida};
  dkCmdBufSetViewports(cmd, 0, &vp, 1);
  static const DkViewportSwizzle kIdentidad = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentidad, 1);
  dkCmdBufSetScissors(cmd, 0, &tijera, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cws);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* shaders[2] = {&vs_salida_, fs};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, kBytesUboRampa);
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(ranura, 0));
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, dkMakeTextureHandle(ranura_tabla, 0));
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  if (volcando_ && REXCVAR_GET(fc3_trace_hdr_chain)) {
    ImagenDk output;
    output.imagen = salida;
    output.formato = DkImageFormat_RGBA8_Unorm;
    output.ancho = ancho_salida;
    output.alto = alto_salida;
    VolcarResuelta(output, 0xFC0000FEu, 6);
  }
  OlvidarEstado();
  return true;
}

bool RenderDk::Presentar(rex::ui::Presenter* presentador, const TexturaSwap& swap, uint32_t ancho, uint32_t alto) {
  const bool fotograma_volcado = volcando_ || volcado_pedido_;  // a dump stalls on purpose: not a hitch
  if (REXCVAR_GET(fc3_dk_esperar_gpu_fotograma)) {
    CerrarTrabajo();
    proveedor_.dispositivo().EsperarInactiva();
  }
  const bool hdr_chain = REXCVAR_GET(fc3_trace_hdr_chain);
  const auto begin_dump = [&]() {
  if (volcado_pedido_) {
    volcado_pedido_ = false;
    if (!volcado_bloque_) {
      volcado_bloque_ = nucleo_.CrearBloqueLectura(kBytesVolcado, "volcado de un fotograma");
      volcado_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(volcado_bloque_));
      volcado_gpu_ = dkMemBlockGetGpuAddr(volcado_bloque_);
    }
    if (volcado_bloque_) {
      dkMemBlockFlushCpuCache(volcado_bloque_, 0, kBytesVolcado);  // nothing of it stays in the CPU's cache
      volcando_ = true;
      volcado_usados_ = 0;
      volcado_.clear();
      volcado_vistos_ = 0;
      REXLOG_INFO("[deko3d] volcado: el proximo fotograma, cada textura resuelta a la SD");
    }
  }
  };
  if (!hdr_chain) {
    if (volcando_) TerminarVolcado();
    begin_dump();
  }
  RexSwitchPerfCount(0);  // the profiler's "juego N fps"
  const uint64_t ahora = deko::AhoraNs();
  if (ultimo_swap_ns_) {
    const double ms = double(ahora - ultimo_swap_ns_) / 1e6;
    const uint32_t cubeta = ms < 15.0    ? 0
                            : ms < 18.0  ? 1
                            : ms < 25.0  ? 2
                            : ms < 30.0  ? 3
                            : ms < 36.0  ? 4
                            : ms < 50.0  ? 5
                            : ms < 60.0  ? 6
                            : ms < 75.0  ? 7
                            : ms < 100.0 ? 8
                            : ms < 150.0 ? 9
                                         : 10;
    ++cubetas_swap_[cubeta];
    peor_swap_ms_ = std::max(peor_swap_ms_, ms);
    hooks_.Latido(ms);
    if (ms > 45.0 && !fotograma_volcado) {
      ++g_tirones_45;
      g_tirones_100 += ms > 100.0 ? 1 : 0;
      g_tirones_ms += ms;
      g_peor_intervalo_ms = std::max(g_peor_intervalo_ms, ms);
      // AhoraNs is the CPU counter (19.2 MHz) times 625/12: back to ticks, the sampler's clock.
      RexSwitchPerfTiron(ultimo_swap_ns_ * 12 / 625, ahora * 12 / 625);
    }
  }
  ultimo_swap_ns_ = ahora;
  RecogerCompilaciones();
  AsegurarTrabajo();
  bool pintado = false;
  bool llamada = false;
  const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
  ultimo_frontal_ = base;  // nfsmw_nativo_frontal_perezoso: the next resolve here may go to the Swap directly
  // The front buffer is still in its render target (and the texture was not written): painted from there.
  const bool desde_destino = frontal_.pendiente && frontal_.base == base;
  const ImagenDk* origen = desde_destino ? frontal_.destino : TexturaResuelta(base);
  if (!origen && !desde_destino) {
    // Fallback: the Swap's frontbuffer address matched no resolved texture. Some titles (e.g. Halo 3) present
    // from an address the Swap's fetch constant does not point at, so the exact lookup finds nothing and the
    // screen stays black even though a frame was resolved. Show the most-recently written resolved frame
    // instead. Titles whose frontbuffer base matches a resolve (NFS) find origen above and never reach this.
    uint32_t mejor_version = 0;
    for (auto& par : resueltas_) {
      if (par.second.escrita && par.second.imagen.version >= mejor_version) {
        mejor_version = par.second.imagen.version;
        origen = &par.second.imagen;
      }
    }
  }
  const uint32_t ancho_origen = desde_destino ? frontal_.ancho : (origen ? origen->ancho : 0);
  const uint32_t alto_origen = desde_destino ? frontal_.alto : (origen ? origen->alto : 0);
  if (!origen) {
    Rechazar(4, "Swap sin textura resuelta");
  } else if (presentador) {
    const uint32_t w = std::min(ancho ? ancho : ancho_origen, ancho_origen);
    const uint32_t h = std::min(alto ? alto : alto_origen, alto_origen);
    presentador->RefreshGuestOutput(w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& c) {
      llamada = true;
      auto& contexto = static_cast<ContextoSalidaDeko3d&>(c);
      contexto.SetIs8bpc(true);
      const ImagenDk* fuente = origen;
      if (desde_destino) {
        // From the target only texel for texel at the same size (it is taller than the front buffer); otherwise, or
        // with FXAA, the copy is made now and painted as always.
        if (hooks_.Fxaa() || contexto.ancho() != w || contexto.alto() != h) {
          MaterializarFrontal();
          fuente = TexturaResuelta(base);
        } else {
          frontal_.pendiente = false;
          frontal_obsoleta_ = base;
        }
      }
      pintado = fuente && PintarSalida(contexto.imagen(), contexto.ancho(), contexto.alto(), *fuente, w, h);
      CerrarTrabajo();  // submitted before returning, as RefreshGuestOutput requires
      return pintado;
    });
  }
  if (!llamada) {
    CerrarTrabajo();
  }
  if (hdr_chain) {
    if (volcando_) TerminarVolcado();
    begin_dump();
  }
  ++fotograma_;
  vigia_fotograma_.store(fotograma_, std::memory_order_relaxed);
  vigia_trabajo_.store(trabajo_, std::memory_order_relaxed);
  uso_fotograma_anterior_ = uso_fotograma_actual_;  // fc3-switch: ExpulsarResueltasViejas
  uso_fotograma_actual_ = uso_resueltas_;
  geometria_.NuevoFotograma(fotograma_);
  PerfilAlCambiarDeFotograma();
  if (pintado) {
    ++presentados_;
  }
  EscribirLecturasTerminadas();
  CerrarFotogramaDeLaGuardiaDelCielo();
  SoltarTexturasSiHaceFalta();
  GuardarCacheSiHaceFalta(false);
  InformePeriodico();
  return pintado;
}

// ---- The DKSH cache file and the report

void RenderDk::GuardarCacheSiHaceFalta(bool ya) {
  const uint64_t ahora = deko::AhoraNs();
  if (!cache_sucia_ || (!ya && ahora - cache_guardada_ns_ < 20'000'000'000ull)) {
    return;
  }
  cache_sucia_ = false;
  cache_guardada_ns_ = ahora;
  // Serialized here (the ring thread owns the cache), written on another thread: ~2 MB to the SD card would be a
  // stutter. Only this console's own entries: the whole merged cache (30 MB with the PC tool's) took the ring thread a
  // copy of it and the SD card a 30 MB write every 20 s while new variants compiled.
  auto datos = std::make_shared<std::vector<uint8_t>>(cache_consola_.Serializar());
  const size_t entradas = cache_consola_.size();
  auto escribir = [datos, entradas] {
    std::error_code error;
    const std::filesystem::path ruta = RutaCache();
    std::filesystem::create_directories(ruta.parent_path(), error);
    const std::filesystem::path temporal = ruta.string() + ".tmp";
    {
      std::ofstream f(temporal, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(datos->data()), std::streamsize(datos->size()));
      if (!f) {
        REXLOG_WARN("[deko3d] no se pudo escribir {}", temporal.string());
        return;
      }
    }
    std::filesystem::rename(temporal, ruta, error);
    if (error) {
      std::filesystem::remove(ruta, error);
      std::filesystem::rename(temporal, ruta, error);
    }
    REXLOG_INFO("[deko3d] cache de DKSH guardada: {} entradas, {} KB", entradas, datos->size() >> 10);
  };
  // One write at a time, on a thread that is joined, never detached: libnx has no pthread_detach, and
  // std::thread::detach() throws (std::system_error, "Function not implemented") and ends the process.
  if (guardando_.joinable()) {
    guardando_.join();  // the previous one, 20 s or more ago
  }
  if (ya) {
    escribir();
  } else {
    guardando_ = std::thread(escribir);
  }
}

void RenderDk::InformePeriodico() {
  const uint64_t ahora = deko::AhoraNs();
  auto tomar = [&] {
    informe_ns_ = ahora;
    informe_dibujos_ = dibujados_;
    informe_fotogramas_ = fotograma_;
    informe_copias_ = copias_;
    informe_borrados_ = borrados_ + borrados_profundidad_;
    informe_barreras_ = barreras_;
    informe_creadas_ = texturas_creadas_;
    informe_subidas_ = texturas_subidas_;
    informe_bytes_subidos_ = bytes_subidos_;
    informe_ns_grabando_ = ns_grabando_;
    informe_lecturas_ = lecturas_escritas_;
    informe_gpu_ns_ = gpu_ns_;
    informe_gpu_trabajos_ = gpu_trabajos_;
    informe_espera_ns_ = nucleo_.NsEsperandoValla();
    informe_saltados_ = borrados_saltados_;
    informe_z_temprana_ = dibujos_z_temprana_;
    informe_categorias_ = gpu_categorias_ns_;
    informe_etapas_ = ns_etapas_;
    informe_dedupe_bytes_ = dedupe_.bytes_ahorrados();
    informe_intercambios_ = intercambios_;
    informe_restauraciones_ = restauraciones_;
    informe_cubos_ = cubos_refrescados_;
    informe_cielos_ = cielo_aplazados_;
    informe_copias_3d_ = copias_3d_;
    informe_barreras_fragmentos_ = barreras_fragmentos_;
    informe_frontales_ = frontales_ahorrados_;
    informe_bandas_ = bandas_completadas_;
    informe_registros_empujados_ = registros_empujados_;
    informe_empujes_ = empujes_;
    informe_geometria_ = geometria_.estadisticas();
    informe_bytes_huella_ = bytes_huella_;
    informe_bytes_ahorrados_muestra_ = bytes_ahorrados_muestra_;
    informe_muestras_aciertos_ = muestras_aciertos_;
    informe_huellas_aplazadas_ = huellas_aplazadas_;
    informe_texturas_reusadas_ = texturas_reusadas_;
  };
  if (!informe_ns_) {
    tomar();
    return;
  }
  if (ahora - informe_ns_ < 10'000'000'000ull) {
    return;
  }
  // Everything per frame or per interval, except the totals marked as such.
  const double segundos = double(ahora - informe_ns_) / 1e9;
  const double fotogramas = std::max(1.0, double(fotograma_ - informe_fotogramas_));
  const uint64_t dibujos = dibujados_ - informe_dibujos_;
  const deko::EstadisticasNucleo n = nucleo_.TomarEstadisticas();
  X360_INFORME(
      "[deko3d] ultimos {:.0f} s: {:.1f} fotogramas/s, {:.0f} dibujos por fotograma ({} rechazados en total); GPU "
      "{:.2f} ms por fotograma ({} trabajos medidos); por fotograma: {:.1f} copias, {:.1f} borrados, {:.1f} barreras de transferencia, {:.2f} MB de subida (vertices, "
      "indices, constantes, texturas); grabar un dibujo {:.2f} us en RenderDk (sin esperas a la GPU); texturas: {} vivas ({} MB), {} creadas "
      "y {} subidas ({} MB) en el intervalo; lecturas de vuelta al invitado {}; {} trabajos, {} cambios de ranura por "
      "subida llena en total, comandos max {} KB; imagenes {} MB en {} trozos; shaders: {} de la cache, {} compilados "
      "en el acto, {} en segundo plano (en total)",
      segundos, fotogramas / segundos, double(dibujos) / fotogramas, rechazados_,
      double(gpu_ns_ - informe_gpu_ns_) / 1e6 / fotogramas, gpu_trabajos_ - informe_gpu_trabajos_,
      double(copias_ - informe_copias_) / fotogramas,
      double(borrados_ + borrados_profundidad_ - informe_borrados_) / fotogramas,
      double(barreras_ - informe_barreras_) / fotogramas,
      n.trabajos ? double(n.bytes_subida) / fotogramas / 1048576.0 : 0.0,
      dibujos ? double(ns_grabando_ - informe_ns_grabando_) / double(dibujos) / 1000.0 : 0.0, texturas_.size(),
      bytes_texturas_ >> 20, texturas_creadas_ - informe_creadas_, texturas_subidas_ - informe_subidas_,
      (bytes_subidos_ - informe_bytes_subidos_) >> 20, lecturas_escritas_ - informe_lecturas_, n.trabajos,
      cambios_ranura_, n.bytes_comandos_max >> 10, n.bytes_imagenes >> 20, n.trozos_imagenes, aciertos_cache_,
      compilados_en_el_acto_, compilados_en_fondo_);
  // The aggregate rejected count alone cannot distinguish missing shaders,
  // invalid vertices and upload exhaustion. These counters are cumulative.
  if (!causas_.empty()) {
    const auto causas = EstadisticasDeDibujos().causas;
    for (size_t i = 0; i < std::min<size_t>(causas.size(), 4); ++i) {
      X360_INFORME("[deko3d] rechazos acumulados: causa {} = {}", causas[i].first, causas[i].second);
    }
  }
  if (recortes_11_ || indice_cero_dibujos_) {
    X360_INFORME("[deko3d] acumulado: causa 11 (recortados) = {}, flujos con indice cero = {}", recortes_11_,
                 indice_cero_dibujos_);
  }
  const auto ms = [&](uint32_t c) { return double(gpu_categorias_ns_[c] - informe_categorias_[c]) / 1e6 / fotogramas; };
  X360_INFORME(
      "[deko3d] por fotograma: GPU sombras {:.2f} ms, escena {:.2f}, reflejo {:.2f}, 320 {:.2f}, menores {:.2f}, copias "
      "{:.2f}, borrados {:.2f}, resto {:.2f}, escena sin profundidad {:.2f}, GPU parada entre trabajos {:.2f}; el anillo "
      "espero a la GPU {:.2f} ms; borrados saltados {:.1f}; dibujos con Z temprana {:.0f}",
      ms(kGpuSombras), ms(kGpuEscena), ms(kGpuReflejo), ms(kGpu320), ms(kGpuMenores), ms(kGpuCopias), ms(kGpuBorrados),
      ms(kGpuOtros), ms(kGpuEscenaSinProfundidad), ms(kGpuHuecoEntreTrabajos),
      double(nucleo_.NsEsperandoValla() - informe_espera_ns_) / 1e6 / fotogramas,
      double(borrados_saltados_ - informe_saltados_) / fotogramas,
      double(dibujos_z_temprana_ - informe_z_temprana_) / fotogramas);
  const auto us = [&](uint32_t e) { return dibujos ? double(ns_etapas_[e] - informe_etapas_[e]) / double(dibujos) / 1e3 : 0.0; };
  static constexpr const char* kFasesSm[3] = {"mirando", "APLICANDO", "apagada"};
  X360_INFORME(
      "[deko3d] por fotograma: {:.2f} intercambios en vez de copias, {:.2f} restauraciones, {:.2f} cubos con caras "
      "resueltas, {:.2f} cielos aplazados (guardia {}), {:.2f} MB de vertices sin copiar (repetidos en el trabajo); "
      "sombra por minimo: fase {}, {} ciclos ({} limpios seguidos, {} aplicados), {} diferencias; por dibujo en el "
      "anillo: preparar {:.2f} us, texturas {:.2f}, estado {:.2f}, vertices {:.2f}, indices {:.2f}, constantes y dibujo "
      "{:.2f}",
      double(intercambios_ - informe_intercambios_) / fotogramas,
      double(restauraciones_ - informe_restauraciones_) / fotogramas,
      double(cubos_refrescados_ - informe_cubos_) / fotogramas, double(cielo_aplazados_ - informe_cielos_) / fotogramas,
      cielo_guardia_ == kCieloAplazando ? "superada" : cielo_guardia_ == kCieloDescartado ? "descartada" : "mirando",
      double(dedupe_.bytes_ahorrados() - informe_dedupe_bytes_) / 1048576.0 / fotogramas, kFasesSm[sm_fase_ % 3],
      sm_ciclos_, sm_limpios_seguidos_, sm_ciclos_aplicados_, sm_diferencias_, us(0), us(1), us(2), us(3), us(4), us(5));
  X360_INFORME(
      "[deko3d] por fotograma: {:.1f} copias en el motor 3D, {:.1f} barreras de fragmentos, {:.2f} frontales pintados "
      "desde su destino sin copiar ({} copiados en total, {}), {:.2f} bandas borradas; constantes por diferencias: "
      "{:.1f} registros y {:.1f} empujes por dibujo",
      double(copias_3d_ - informe_copias_3d_) / fotogramas,
      double(barreras_fragmentos_ - informe_barreras_fragmentos_) / fotogramas,
      double(frontales_ahorrados_ - informe_frontales_) / fotogramas, frontales_copiados_,
      frontal_apagado_ ? "APAGADO por una lectura tardia" : "activo", double(bandas_completadas_ - informe_bandas_) / fotogramas,
      dibujos ? double(registros_empujados_ - informe_registros_empujados_) / double(dibujos) : 0.0,
      dibujos ? double(empujes_ - informe_empujes_) / double(dibujos) : 0.0);
  X360_INFORME(
      "[deko3d] comprobacion de texturas: por fotograma {:.2f} MB leidos para sus huellas, {:.2f} MB ahorrados por "
      "muestreo ({:.1f} recomprobaciones solo con la muestra), {:.1f} aplazadas por el presupuesto; muestreo {} ({} "
      "con las dos huellas, {} muestras distintas en total); {:.0f} % de los dibujos reusan las texturas del anterior",
      double(bytes_huella_ - informe_bytes_huella_) / 1048576.0 / fotogramas,
      double(bytes_ahorrados_muestra_ - informe_bytes_ahorrados_muestra_) / 1048576.0 / fotogramas,
      double(muestras_aciertos_ - informe_muestras_aciertos_) / fotogramas,
      double(huellas_aplazadas_ - informe_huellas_aplazadas_) / fotogramas,
      muestreo_huellas_ > 0 ? "activo" : muestreo_huellas_ == 0 ? "APAGADO" : "sin empezar", muestras_comprobadas_,
      muestras_distintas_, dibujos ? 100.0 * double(texturas_reusadas_ - informe_texturas_reusadas_) / double(dibujos) : 0.0);
  if (geometria_.activo()) {
    const deko::AlmacenGeometria::Estadisticas& g = geometria_.estadisticas();
    const deko::AlmacenGeometria::Estadisticas& a = informe_geometria_;
    const auto mb = [&](uint64_t ahora_b, uint64_t antes_b) { return double(ahora_b - antes_b) / 1048576.0 / fotogramas; };
    X360_INFORME(
        "[deko3d] geometria entre fotogramas: por fotograma {:.2f} MB sin copiar ({:.0f} de {:.0f} consultas), {:.2f} MB "
        "con huella, {:.3f} MB guardados ({:.1f} rangos), {:.1f} cambios, {:.1f} al anillo sin huella (cambiantes); "
        "{} MB de {} usados, {} rangos conocidos; {} desalojos, {} sin sitio, {} colisiones (en el intervalo); "
        "muestreo {}: {:.0f} comprobaciones por muestra por fotograma, {} desacuerdos en total",
        mb(g.bytes_aciertos, a.bytes_aciertos), double(g.aciertos - a.aciertos) / fotogramas,
        double(g.consultas - a.consultas) / fotogramas, mb(g.bytes_huella, a.bytes_huella),
        mb(g.bytes_admitidos, a.bytes_admitidos), double(g.admitidas - a.admitidas) / fotogramas,
        double(g.cambios - a.cambios) / fotogramas, double(g.volatiles - a.volatiles) / fotogramas,
        geometria_.bytes_usados() >> 20, geometria_.bytes_total() >> 20, geometria_.entradas(),
        g.desalojos - a.desalojos, g.sin_sitio - a.sin_sitio, g.colisiones - a.colisiones,
        geometria_.muestreo() ? (g.muestreos > 20000 ? "activo" : "con la guardia") : "APAGADO",
        double(g.muestreos - a.muestreos) / fotogramas, g.desacuerdos);
  }
  X360_INFORME(
      "[deko3d] tirones: {} intervalos entre swaps de mas de 45 ms ({:.0f} ms en total), {} de mas de 100 ms, el peor "
      "{:.1f} ms (sin contar volcados)",
      g_tirones_45, g_tirones_ms, g_tirones_100, g_peor_intervalo_ms);
  g_tirones_45 = 0;
  g_tirones_100 = 0;
  g_tirones_ms = 0.0;
  g_peor_intervalo_ms = 0.0;
  tomar();
}

// ---- The GPU profile by draw (nfsmw_dk_perfil_gpu_s)

void RenderDk::PerfilAlCambiarDeFotograma() {
  if (!perfil_bloque_) {
    return;
  }
  if (perfilando_) {
    // The profiled frame ended: its works are read once the GPU has done them.
    perfilando_ = false;
    perfil_pendiente_ = !perfil_marcas_.empty();
  }
  if (perfil_pendiente_) {
    // Its last work is submitted (the frame closed it); its fence then is its own. Not while it is still the one
    // being recorded.
    if ((nucleo_.TrabajoAbierto() && perfil_trabajo_ == trabajo_) || !nucleo_.TrabajoTerminado(perfil_trabajo_)) {
      return;
    }
    InformeDelPerfil();
    InformeDeCopias();
    perfil_pendiente_ = false;
    perfil_marcas_.clear();
    marcas_copia_.clear();
  }
  const uint64_t ahora = deko::AhoraNs();
  if (ahora - perfil_ns_ >= uint64_t(REXCVAR_GET(nfsmw_dk_perfil_gpu_s)) * 1'000'000'000ull) {
    perfil_ns_ = ahora;
    perfilando_ = true;
    perfil_fotograma_ = fotograma_;
    perfil_marcas_.clear();
    marcas_copia_.clear();
  }
}

void RenderDk::InformeDelPerfil() {
  dkMemBlockFlushCpuCache(perfil_bloque_, 0, uint32_t(perfil_marcas_.size()) * 32);
  static constexpr const char* kCategorias[kGpuCategorias] = {"otros",   "sombras", "escena",  "reflejo",
                                                              "320",     "menores", "copias",  "borrados",
                                                              "escena sin Z", "hueco"};
  struct Suma {
    double ns = 0;
    uint32_t dibujos = 0;
    uint64_t vertices = 0;
  };
  struct Pase {
    Suma suma;
    uint32_t categoria = 0, ancho = 0, alto = 0, orden = 0;
    uint64_t color = 0, profundidad = 0;
  };
  std::unordered_map<uint64_t, Pase> pases;
  std::unordered_map<const DkShader*, Suma> por_ps, por_vs;
  std::array<Suma, kGpuCategorias> por_categoria{};
  double total = 0;
  for (size_t i = 0; i < perfil_marcas_.size(); ++i) {
    uint64_t t0 = 0, t1 = 0;
    std::memcpy(&t0, perfil_cpu_ + i * 32 + 8, sizeof(t0));
    std::memcpy(&t1, perfil_cpu_ + i * 32 + 24, sizeof(t1));
    const double ns = t1 >= t0 ? double(t1 - t0) * deko::kNsPorUnidadGpu : 0.0;
    const MarcaPerfil& m = perfil_marcas_[i];
    if (m.repeticion) {
      continue;  // the repeat test's extra copy: not part of the frame
    }
    total += ns;
    auto sumar = [&](Suma& s) {
      s.ns += ns;
      ++s.dibujos;
      s.vertices += m.vertices;
    };
    auto [pase, nuevo] = pases.try_emplace(m.pase);
    if (nuevo) {
      pase->second.categoria = m.categoria;
      pase->second.ancho = m.ancho;
      pase->second.alto = m.alto;
      pase->second.color = m.color;
      pase->second.profundidad = m.profundidad;
      pase->second.orden = uint32_t(pases.size());
    }
    sumar(pase->second.suma);
    sumar(por_ps[m.ps]);
    sumar(por_vs[m.vs]);
    sumar(por_categoria[std::min<uint32_t>(m.categoria, kGpuCategorias - 1)]);
  }
  std::string categorias;
  for (uint32_t c = 0; c < kGpuCategorias; ++c) {
    if (por_categoria[c].dibujos) {
      categorias += fmt::format("{}{} {:.2f} ms en {} dibujos", categorias.empty() ? "" : ", ", kCategorias[c],
                                por_categoria[c].ns / 1e6, por_categoria[c].dibujos);
    }
  }
  REXLOG_INFO("[deko3d] perfil GPU del fotograma {}: {} dibujos medidos uno a uno, {:.2f} ms dentro de los dibujos "
              "({} pases); por categoria: {}",
              perfil_fotograma_, perfil_marcas_.size(), total / 1e6, pases.size(), categorias);
  // The passes that cost most, in frame order within the top.
  std::vector<const Pase*> orden_pases;
  for (const auto& [serie, pase] : pases) {
    orden_pases.push_back(&pase);
  }
  std::sort(orden_pases.begin(), orden_pases.end(), [](const Pase* a, const Pase* b) { return a->suma.ns > b->suma.ns; });
  for (size_t i = 0; i < std::min<size_t>(orden_pases.size(), 12); ++i) {
    const Pase& p = *orden_pases[i];
    REXLOG_INFO("[deko3d] perfil GPU: pase {} del fotograma ({}, {}x{}, color {:016X}, Z {:016X}): {:.2f} ms, {} "
                "dibujos, {} vertices o indices",
                p.orden, kCategorias[std::min<uint32_t>(p.categoria, kGpuCategorias - 1)], p.ancho, p.alto, p.color,
                p.profundidad, p.suma.ns / 1e6, p.suma.dibujos, p.suma.vertices);
  }
  // The most expensive single draws, with what decides their cost (pixels, textures). The extra copies of the repeat
  // test follow their draw in the marks: listed with it, not on their own.
  auto ms_de = [&](size_t i) {
    uint64_t t0 = 0, t1 = 0;
    std::memcpy(&t0, perfil_cpu_ + i * 32 + 8, sizeof(t0));
    std::memcpy(&t1, perfil_cpu_ + i * 32 + 24, sizeof(t1));
    return t1 >= t0 ? double(t1 - t0) * deko::kNsPorUnidadGpu / 1e6 : 0.0;
  };
  std::vector<std::pair<double, size_t>> dibujos;
  for (size_t i = 0; i < perfil_marcas_.size(); ++i) {
    if (!perfil_marcas_[i].repeticion) {
      dibujos.emplace_back(ms_de(i), i);
    }
  }
  std::sort(dibujos.begin(), dibujos.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  static constexpr const char* kVeces[5] = {"", "otra vez igual", "con el sampler 0", "con texturas vacias de 1x1",
                                            "escribiendo solo el destino 0"};
  for (size_t k = 0; k < std::min<size_t>(dibujos.size(), 8); ++k) {
    const size_t i = dibujos[k].second;
    const MarcaPerfil& m = perfil_marcas_[i];
    const auto it = datos_shaders_.find(m.ps);
    std::string pruebas;
    for (size_t j = i + 1; j < perfil_marcas_.size() && perfil_marcas_[j].repeticion; ++j) {
      pruebas += fmt::format("; {} {:.2f} ms", kVeces[std::min<uint32_t>(perfil_marcas_[j].repeticion, 4)], ms_de(j));
    }
    REXLOG_INFO("[deko3d] perfil GPU: dibujo {} ({:.2f} ms): PS {:016X} (n{}), pase {} {}x{}, viewport {:.0f}x{:.0f}, "
                "tijera {}x{}, mascaras {:04X}, RB_DEPTHCONTROL {:08X}, {} vertices o indices; {} texturas, la primera "
                "{}x{} formato {} sampler {}{}",
                i, dibujos[k].first, it != datos_shaders_.end() ? it->second.huella : 0,
                it != datos_shaders_.end() ? it->second.numero : 0,
                kCategorias[std::min<uint32_t>(m.categoria, kGpuCategorias - 1)], m.ancho, m.alto, m.vp_ancho,
                m.vp_alto, m.tijera_ancho, m.tijera_alto, m.mascaras, m.profundidad_control, m.vertices, m.texturas,
                m.tex_ancho, m.tex_alto, m.tex_formato, m.tex_sampler, pruebas);
  }
  auto shaders = [&](const std::unordered_map<const DkShader*, Suma>& mapa, const char* que, size_t cuantos) {
    std::vector<std::pair<const DkShader*, Suma>> lista(mapa.begin(), mapa.end());
    std::sort(lista.begin(), lista.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    for (size_t i = 0; i < std::min(lista.size(), cuantos); ++i) {
      const auto& [shader, suma] = lista[i];
      const auto it = datos_shaders_.find(shader);
      const DatosShaderDk d = it != datos_shaders_.end() ? it->second : DatosShaderDk{};
      REXLOG_INFO("[deko3d] perfil GPU: {} {:016X} (n{}, espec {:08X}, variante {:X}): {:.2f} ms en {} dibujos ({:.1f} "
                  "us por dibujo, {} vertices o indices); uam: {} registros, {} bytes de codigo, scratch {}",
                  que, d.huella, d.numero, d.espec, d.variante, suma.ns / 1e6, suma.dibujos,
                  suma.dibujos ? suma.ns / 1e3 / suma.dibujos : 0.0, suma.vertices, d.registros, d.bytes_codigo,
                  d.scratch);
    }
  };
  shaders(por_ps, "PS", 15);
  shaders(por_vs, "VS", 6);
}

// fc3-switch 2026-11-07 DIAGNOSTIC (hang hunt): every 250 ms checks that frames advance; a frame counter standing
// still for more than 1.5 s logs "[watchdog] GPU stall" (once per stall) with what the render thread did last. Once a
// second a heartbeat line (frame, fps, process memory) so the last point before a hang is in the PC log.
void RenderDk::Vigia() {
  uint64_t ultimo = vigia_fotograma_.load(), ultimo_latido = ultimo;
  uint64_t desde = deko::AhoraNs(), latido_ns = desde;
  bool avisado = false;
  while (!vigia_fin_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t ahora = deko::AhoraNs();
    const uint64_t f = vigia_fotograma_.load(std::memory_order_relaxed);
    if (f != ultimo) {
      if (avisado) {
        REXLOG_WARN("[watchdog] frames move again: frame {} after {:.1f} s", f, double(ahora - desde) / 1e9);
      }
      ultimo = f;
      desde = ahora;
      avisado = false;
    } else if (!avisado && f > 1 && ahora - desde > 1'500'000'000ull) {
      avisado = true;
      REXLOG_WARN("[watchdog] GPU stall: frame {} stands for {:.1f} s; work {}; last draw #{} VS {} PS {}; last copy "
                  "'{}'; last fetch0 binding draw #{} addr {:08X} bytes {}",
                  f, double(ahora - desde) / 1e9, vigia_trabajo_.load(), vigia_dibujos_.load(), vigia_vs_.load(),
                  vigia_ps_.load(), vigia_copia_.load(), vigia_fetch0_dibujo_.load(), vigia_fetch0_dir_.load(),
                  vigia_fetch0_bytes_.load());
    }
    if (ahora - latido_ns >= 1'000'000'000ull) {
      uint64_t usada = 0, total = 0;
#if defined(__SWITCH__)
      svcGetInfo(&usada, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
#endif
      REXLOG_INFO("[heartbeat] frame {} fps {:.1f} draws {} VS {} PS {} copy '{}' fetch0 draws {} mem {}/{} MB", f,
                  double(f - ultimo_latido) * 1e9 / double(ahora - latido_ns), vigia_dibujos_.load(), vigia_vs_.load(),
                  vigia_ps_.load(), vigia_copia_.load(), indice_cero_dibujos_, usada >> 20, total >> 20);
      ultimo_latido = f;
      latido_ns = ahora;
    }
  }
}

int32_t RenderDk::PerfilCopiaInicio(DkCmdBuf cmd) {
  if (!perfilando_ || !copias_bloque_ || marcas_copia_.size() >= kMarcasCopia) {
    return -1;
  }
  const int32_t n = int32_t(marcas_copia_.size());
  marcas_copia_.push_back({"?", 0, 0, 0, 0, 0, 0, 0});  // filled by PerfilCopiaFin
  // As for the first draw of a pass (nfsmw_dk_perfil_barrera): earlier work is not counted as the copy's.
  if (REXCVAR_GET(nfsmw_dk_perfil_barrera)) {
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
  }
  dkCmdBufReportCounter(cmd, DkCounter_Timestamp, copias_gpu_ + DkGpuAddr(n) * 32);
  return n;
}

void RenderDk::PerfilCopiaFin(DkCmdBuf cmd, int32_t n, MarcaCopia m) {
  if (n < 0 || size_t(n) >= marcas_copia_.size()) {
    return;
  }
  if (REXCVAR_GET(nfsmw_dk_perfil_barrera)) {
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);  // the copy's own work is done before its end mark
  }
  dkCmdBufReportCounter(cmd, DkCounter_Timestamp, copias_gpu_ + DkGpuAddr(n) * 32 + 16);
  marcas_copia_[size_t(n)] = m;
  perfil_trabajo_ = trabajo_;
}

void RenderDk::InformeDeCopias() {
  if (!copias_bloque_ || marcas_copia_.empty()) {
    return;
  }
  dkMemBlockFlushCpuCache(copias_bloque_, 0, uint32_t(marcas_copia_.size()) * 32);
  std::vector<std::pair<double, size_t>> lista;
  std::vector<std::pair<std::string, std::pair<double, uint32_t>>> por_tipo;
  double total = 0;
  for (size_t i = 0; i < marcas_copia_.size(); ++i) {
    uint64_t t0 = 0, t1 = 0;
    std::memcpy(&t0, copias_cpu_ + i * 32 + 8, sizeof(t0));
    std::memcpy(&t1, copias_cpu_ + i * 32 + 24, sizeof(t1));
    const double ms = t1 >= t0 ? double(t1 - t0) * deko::kNsPorUnidadGpu / 1e6 : 0.0;
    lista.emplace_back(ms, i);
    const std::string tipo = marcas_copia_[i].tipo;
    auto it = std::find_if(por_tipo.begin(), por_tipo.end(), [&](const auto& e) { return e.first == tipo; });
    if (it == por_tipo.end()) {
      por_tipo.push_back({tipo, {0.0, 0}});
      it = por_tipo.end() - 1;
    }
    it->second.first += ms;
    ++it->second.second;
    total += ms;
  }
  std::string tipos;
  for (const auto& [tipo, s] : por_tipo) {
    tipos += fmt::format("{}{} {:.2f} ms en {}", tipos.empty() ? "" : ", ", tipo, s.first, s.second);
  }
  REXLOG_INFO("[deko3d] perfil copias del fotograma {}: {} copias medidas, {:.2f} ms; por tipo: {}", perfil_fotograma_,
              marcas_copia_.size(), total, tipos);
  std::sort(lista.begin(), lista.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t k = 0; k < std::min<size_t>(lista.size(), 15); ++k) {
    const MarcaCopia& m = marcas_copia_[lista[k].second];
    REXLOG_INFO("[deko3d] perfil copias: copia {} {} {}x{} fmt {} -> {}x{} fmt {} ({} KB): {:.2f} ms", lista[k].second,
                m.tipo, m.origen_ancho, m.origen_alto, m.origen_formato, m.destino_ancho, m.destino_alto,
                m.destino_formato, m.bytes / 1024, lista[k].first);
  }
}

// ---- Statistics for the system's report

// ---- Occlusion queries

void RenderDk::EmpezarOclusion(uint32_t base) {
  LeerOclusionesTerminadas();
  // Every pair may still be waited for (the GPU far behind): this query is not counted, the game gets "visible".
  if (oclusion_pendientes_.size() + oclusion_abiertas_.size() + 1 >= kParesOclusion) {
    ++oclusion_stats_[1];
    oclusion_abiertas_.erase(base);
    return;
  }
  AsegurarTrabajo();
  ConsultaDk c;
  c.par = oclusion_siguiente_;
  c.dibujados = dibujados_;
  oclusion_siguiente_ = (oclusion_siguiente_ + 1) % kParesOclusion;
  dkCmdBufReportCounter(nucleo_.comandos(), DkCounter_SamplesPassed, oclusion_gpu_ + uint64_t(c.par) * 32);
  oclusion_abiertas_[base] = c;
}

uint64_t RenderDk::TerminarOclusionAsincrona(uint32_t base) {
  const auto it = oclusion_abiertas_.find(base);
  if (it == oclusion_abiertas_.end()) {
    return 0;
  }
  ConsultaDk c = it->second;
  oclusion_abiertas_.erase(it);
  // Its box was not drawn (a shader the library lacks, a skipped draw): no count, the game sees it visible.
  if (dibujados_ == c.dibujados || !nucleo_.TrabajoAbierto()) {
    return 0;
  }
  dkCmdBufReportCounter(nucleo_.comandos(), DkCounter_SamplesPassed, oclusion_gpu_ + uint64_t(c.par) * 32 + 16);
  c.ticket = ++oclusion_ticket_;
  c.trabajo = trabajo_;
  oclusion_pendientes_.push_back(c);
  ++oclusion_stats_[0];
  return c.ticket;
}

bool RenderDk::TerminarOclusion(uint32_t base, uint64_t& muestras) {
  muestras = 0;
  const uint64_t ticket = TerminarOclusionAsincrona(base);
  if (!ticket) {
    return false;  // not counted (no draw inside, no room): the caller answers "visible"
  }
  if (nucleo_.TrabajoAbierto()) {
    CerrarTrabajo();
  }
  proveedor_.dispositivo().EsperarInactiva();
  return ResultadoOclusion(ticket, muestras);
}

bool RenderDk::ResultadoOclusion(uint64_t ticket, uint64_t& muestras) {
  LeerOclusionesTerminadas();
  const auto it = oclusion_listas_.find(ticket);
  if (it == oclusion_listas_.end()) {
    return false;
  }
  muestras = it->second;
  oclusion_listas_.erase(it);
  ++oclusion_stats_[2];
  oclusion_stats_[3] += muestras;
  oclusion_stats_[4] = std::max(oclusion_stats_[4], muestras);
  return true;
}

void RenderDk::EnviarOclusiones() {
  if (nucleo_.TrabajoAbierto() && !oclusion_pendientes_.empty() && oclusion_pendientes_.back().trabajo == trabajo_) {
    CerrarTrabajo();  // the next draw opens another
  }
}

void RenderDk::LeerOclusionesTerminadas() {
  while (!oclusion_pendientes_.empty()) {
    const ConsultaDk& c = oclusion_pendientes_.front();
    if ((nucleo_.TrabajoAbierto() && c.trabajo == trabajo_) || !nucleo_.TrabajoTerminado(c.trabajo)) {
      break;
    }
    dkMemBlockFlushCpuCache(oclusion_bloque_, c.par * 32, 32);
    uint64_t inicio = 0, fin = 0;
    std::memcpy(&inicio, oclusion_cpu_ + size_t(c.par) * 32, 8);
    std::memcpy(&fin, oclusion_cpu_ + size_t(c.par) * 32 + 16, 8);
    oclusion_listas_[c.ticket] = fin >= inicio ? fin - inicio : 0;
    oclusion_pendientes_.pop_front();
  }
  // Tickets the system gave up on (it answered "visible" after waiting too long): forgotten.
  if (oclusion_listas_.size() > kParesOclusion) {
    for (auto it = oclusion_listas_.begin(); it != oclusion_listas_.end();) {
      it = it->first + kParesOclusion < oclusion_ticket_ ? oclusion_listas_.erase(it) : std::next(it);
    }
  }
}

void RenderDk::EstadisticasOclusion(uint64_t valores[5]) const { std::copy(oclusion_stats_, oclusion_stats_ + 5, valores); }

EstadisticasDibujos RenderDk::EstadisticasDeDibujos() const {
  EstadisticasDibujos e;
  e.dibujados = dibujados_;
  e.rechazados = rechazados_;
  e.texturas = texturas_.size();
  e.subidas_textura = texturas_subidas_;
  e.megas_texturas = bytes_texturas_ >> 20;
  for (const auto& [causa, veces] : causas_) {
    e.causas.emplace_back(causa, veces);
  }
  std::sort(e.causas.begin(), e.causas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  return e;
}

void RenderDk::Estadisticas(uint64_t& copias, uint64_t& borrados, uint64_t& presentados, uint64_t& rechazos) const {
  copias = copias_;
  borrados = borrados_;
  presentados = presentados_;
  rechazos = rechazados_;
}

void RenderDk::TiempoGpu(uint64_t& nanosegundos, uint64_t& trabajos) const {
  nanosegundos = gpu_ns_;
  trabajos = gpu_trabajos_;
}
void RenderDk::EsperasGpu(uint64_t& veces, uint64_t& nanosegundos) const { veces = nanosegundos = 0; }
void RenderDk::CosteGrabar(uint64_t coste[6]) const { std::fill(coste, coste + 6, 0); }
void RenderDk::EstadisticasPipeline(std::array<uint64_t, kGpuCategorias>& fragmentos,
                                    std::array<uint64_t, kGpuCategorias>& vertices,
                                    std::array<uint64_t, kGpuCategorias>& primitivas) const {
  fragmentos.fill(0);
  vertices.fill(0);
  primitivas.fill(0);
}
void RenderDk::CopiasPorTamano(std::array<uint64_t, 4>& copias, std::array<uint64_t, 4>& pixeles) const {
  copias.fill(0);
  pixeles.fill(0);
}
void RenderDk::EstadisticasPorShader(std::vector<uint64_t>& fragmentos, std::vector<uint64_t>& dibujos,
                                     uint64_t& fotogramas) const {
  fragmentos.clear();
  dibujos.clear();
  fotogramas = 0;
}
void RenderDk::IntervalosEntreSwaps(std::array<uint64_t, kCubetasSwap>& cubetas, uint64_t& solapes,
                                    double& peor_ms) const {
  cubetas = cubetas_swap_;
  solapes = 0;
  peor_ms = peor_swap_ms_;
}
void RenderDk::CostePresentar(uint64_t coste[12]) const { std::fill(coste, coste + 12, 0); }

std::unique_ptr<DestinosNativos> CrearDestinosDeko3d(deko::ProveedorDeko3d* proveedor, rex::memory::Memory* memoria,
                                                    rex::x360::RenderHooks& hooks) {
  if (!proveedor) {
    return nullptr;
  }
  return std::make_unique<RenderDk>(*proveedor, memoria, hooks);
}


// ---- The frame dump (PedirVolcado)

void RenderDk::VolcarCopiaSiPedida(uint32_t base, const ImagenDk& imagen, uint32_t formato_guest) {
  if (!volcando_) {
    volcado_copias_n_ = 0;
    return;
  }
  static const std::string pedida = REXCVAR_GET(fc3_volcado_copias_en);
  if (pedida.empty()) return;
  const uint32_t direccion = uint32_t(std::strtoul(pedida.c_str(), nullptr, 16)) & 0x1FFFFFFF;
  if ((base & 0x1FFFFFFF) != direccion) return;
  VolcarResuelta(imagen, 0xFD000000u | (volcado_copias_n_++ & 0xFFF), formato_guest);
}


void RenderDk::Sondear(const ImagenDk& imagen, uint32_t paso, uint32_t ps, uint32_t vs, uint32_t base, uint32_t cual) {
  uint32_t bpp = 0;
  switch (imagen.formato) {
    case DkImageFormat_RGBA8_Unorm:
    case DkImageFormat_RGB10A2_Unorm:
    case DkImageFormat_R32_Float:
    case DkImageFormat_RG16_Float:
    case DkImageFormat_Z24S8:
    case DkImageFormat_RGBA8_Unorm_sRGB:
      bpp = 4;
      break;
    case DkImageFormat_RGBA16_Float:
    case DkImageFormat_RG32_Float:
      bpp = 8;
      break;
    default:
      return;
  }
  // Points of a 1280x704 screen: inside the house block, left of it, above it; scaled to the target's size.
  static const uint32_t puntos[3][2] = {{600, 385}, {250, 385}, {600, 150}};
  for (uint32_t k = 0; k < 3; ++k) {
    if (sonda_.size() >= 60000 || volcado_usados_ + 1024 > kBytesVolcado) return;
    const uint32_t w = std::min(8u, imagen.ancho), h = std::min(8u, imagen.alto);
    const uint32_t x = std::min(puntos[k][0] * imagen.ancho / 1280, imagen.ancho - w);
    const uint32_t y = std::min(puntos[k][1] * imagen.alto / 704, imagen.alto - h);
    AntesDeTransferir();
    const DkImageView vista = VistaCompleta(imagen.imagen);
    const DkImageRect rect = {x, y, 0, w, h, 1};
    const DkCopyBuf destino = {volcado_gpu_ + volcado_usados_, w * bpp, w * h * bpp};
    dkCmdBufCopyImageToBuffer(nucleo_.comandos(), &vista, &rect, &destino, 0);
    sonda_.push_back({volcado_usados_, paso, ps, vs, w, h, bpp, base, cual * 4 + k, x, y, imagen.formato});
    volcado_usados_ += (w * h * bpp + 255) & ~255u;
  }
}

void RenderDk::VolcarResuelta(const ImagenDk& imagen, uint32_t base, uint32_t formato_guest) {
  if (REXCVAR_GET(fc3_sonda)) {  // probe the resolved image instead of copying it whole
    Sondear(imagen, sonda_paso_, 0xFFFF, 0xFFFF, base, 2);
    return;
  }
  if ((REXCVAR_GET(fc3_trace_lighting_checkpoint) || REXCVAR_GET(fc3_trace_hdr_chain)) && (base >> 24) != 0xFC) return;
  uint32_t bpp = 0;
  switch (imagen.formato) {
    case DkImageFormat_RGBA8_Unorm:
    case DkImageFormat_RGB10A2_Unorm:
    case DkImageFormat_R32_Float:
    case DkImageFormat_RG16_Float:
    case DkImageFormat_Z24S8:
      bpp = 4;
      break;
    case DkImageFormat_RGBA16_Float:
    case DkImageFormat_RG32_Float:
      bpp = 8;
      break;
    default:
      return;
  }
  const uint32_t bytes = imagen.ancho * imagen.alto * bpp;
  // fc3-switch (DIAGNOSTIC): each base and size once per dump, and the first fc3_volcado_omitir left out, so the
  // later passes (lighting) fit in the block.
  for (const EntradaVolcado& e : volcado_) {
    if (e.base == base && e.ancho == imagen.ancho && e.alto == imagen.alto && e.formato == imagen.formato) return;
  }
  if (volcado_vistos_++ < uint32_t(REXCVAR_GET(fc3_volcado_omitir))) {
    return;
  }
  if (volcado_.size() >= 64 || volcado_usados_ + bytes > kBytesVolcado) {
    return;
  }
  AntesDeTransferir();
  const DkImageView vista = VistaCompleta(imagen.imagen);
  const DkImageRect rect = {0, 0, 0, imagen.ancho, imagen.alto, 1};
  const DkCopyBuf destino = {volcado_gpu_ + volcado_usados_, imagen.ancho * bpp, bytes};
  dkCmdBufCopyImageToBuffer(nucleo_.comandos(), &vista, &rect, &destino, 0);
  volcado_.push_back({volcado_usados_, imagen.ancho, imagen.alto, bpp, base, formato_guest, imagen.formato});
  volcado_usados_ += (bytes + 255) & ~255u;
}

void RenderDk::TerminarVolcado() {
  volcando_ = false;
  CerrarTrabajo();
  proveedor_.dispositivo().EsperarInactiva();
  dkMemBlockFlushCpuCache(volcado_bloque_, 0, std::max<uint32_t>(volcado_usados_, 256));  // what the GPU wrote
  if (REXCVAR_GET(fc3_sonda)) {
    std::vector<uint8_t> bin(volcado_cpu_, volcado_cpu_ + volcado_usados_);
    std::string txt = "n paso ps vs ancho alto bpp base cual x y formato offset\n";
    for (size_t n = 0; n < sonda_.size(); ++n) {
      const EntradaSonda& e = sonda_[n];
      txt += fmt::format("{} {} {} {} {} {} {} {:08X} {} {} {} {} {}\n", n, e.paso, e.ps, e.vs, e.ancho, e.alto, e.bpp,
                         e.base, e.cual, e.x, e.y, uint32_t(e.formato), e.offset);
    }
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    const auto pbin = carpeta / fmt::format("sonda_{:02}.bin", volcados_);
    const auto ptxt = carpeta / fmt::format("sonda_{:02}.txt", volcados_);
    ++volcados_;
    REXLOG_INFO("[sonda] {} probes, {} bytes -> {}", sonda_.size(), bin.size(), pbin.string());
    sonda_.clear();
    sonda_paso_ = 0;
    volcado_.clear();
    EncolarEscrituraSd([pbin, ptxt, bin = std::move(bin), txt = std::move(txt)]() {
      if (FILE* f = std::fopen(pbin.string().c_str(), "wb")) {
        std::fwrite(bin.data(), 1, bin.size(), f);
        std::fclose(f);
      }
      if (FILE* f = std::fopen(ptxt.string().c_str(), "wb")) {
        std::fwrite(txt.data(), 1, txt.size(), f);
        std::fclose(f);
      }
      REXLOG_INFO("[sonda] written {}", pbin.string());
    });
    return;
  }
  char nombre[128];
  std::snprintf(nombre, sizeof(nombre), "volcado_%02u", volcados_++);
  const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder() / nombre;
  std::error_code ec;
  std::filesystem::create_directories(carpeta, ec);
  // The bytes are copied out here (the block is reused by the next dump); converting and writing them go to a thread of
  // their own, so the ring thread (and the game waiting on it) does not stop for the SD card.
  std::vector<std::vector<uint8_t>> copias(volcado_.size());
  for (size_t n = 0; n < volcado_.size(); ++n) {
    const EntradaVolcado& e = volcado_[n];
    copias[n].assign(volcado_cpu_ + e.offset, volcado_cpu_ + e.offset + size_t(e.ancho) * e.alto * e.bpp);
  }
  REXLOG_INFO("[deko3d] volcado: {} texturas resueltas copiadas, se escriben en {}", volcado_.size(), carpeta.string());
  const bool raw = REXCVAR_GET(fc3_trace_hdr_chain) || REXCVAR_GET(fc3_volcado_raw);
  const uint64_t frame = fotograma_;
  EncolarEscrituraSd([entradas = std::move(volcado_), copias = std::move(copias), carpeta, raw, frame]() {
  char nombre[128];
  std::ofstream manifest;
  if (raw) {
    manifest.open(carpeta / "manifest.json");
    manifest << "{\"frame\":" << frame << ",\"textures\":[";
  }
  for (size_t n = 0; n < entradas.size(); ++n) {
    const EntradaVolcado& e = entradas[n];
    const uint8_t* datos = copias[n].data();
    if (raw) {
      const std::string file = fmt::format("{:02}_{:08X}_{}x{}_f{}_b{}.bin", n, e.base, e.ancho, e.alto,
                                           uint32_t(e.formato), e.bpp);
      std::ofstream binary(carpeta / file, std::ios::binary);
      binary.write(reinterpret_cast<const char*>(datos), std::streamsize(copias[n].size()));
      manifest << (n ? "," : "") << fmt::format("{{\"file\":\"{}\",\"base\":{},\"width\":{},\"height\":{},\"format\":{},\"guest_format\":{},\"bpp\":{},\"bytes\":{}}}",
          file, e.base, e.ancho, e.alto, uint32_t(e.formato), e.formato_guest, e.bpp, copias[n].size());
    }
    const size_t texels = size_t(e.ancho) * e.alto;
    std::vector<float> c(texels * 4, 0.0f);
    bool alfa = false;
    for (size_t i = 0; i < texels; ++i) {
      float* t = &c[i * 4];
      uint32_t v = 0;
      std::memcpy(&v, datos + i * e.bpp, 4);
      switch (e.formato) {
        case DkImageFormat_RGBA8_Unorm:
          for (int k = 0; k < 4; ++k) t[k] = float((v >> (8 * k)) & 0xFF) / 255.0f;
          alfa = true;
          break;
        case DkImageFormat_RGB10A2_Unorm:
          for (int k = 0; k < 3; ++k) t[k] = float((v >> (10 * k)) & 0x3FF) / 1023.0f;
          t[3] = float(v >> 30) / 3.0f;
          alfa = true;
          break;
        case DkImageFormat_R32_Float:
          std::memcpy(&t[0], &v, 4);
          t[1] = t[2] = t[0];
          break;
        case DkImageFormat_RG16_Float:
          t[0] = MedioAFloat(uint16_t(v));
          t[1] = MedioAFloat(uint16_t(v >> 16));
          break;
        case DkImageFormat_RGBA16_Float: {
          uint16_t h[4];
          std::memcpy(h, datos + i * 8, 8);
          for (int k = 0; k < 4; ++k) t[k] = MedioAFloat(h[k]);
          alfa = true;
          break;
        }
        case DkImageFormat_RG32_Float:
          std::memcpy(&t[0], datos + i * 8, 4);
          std::memcpy(&t[1], datos + i * 8 + 4, 4);
          break;
        case DkImageFormat_Z24S8:
          alfa = true;  // Preserve the stencil plane in the diagnostic output.
          t[0] = float(v >> 8);  // Maxwell Z24S8: depth in bits 8-31, stencil in bits 0-7.
          t[3] = float(v & 0xFF) / 255.0f;
          break;
        default:
          break;
      }
    }
    // Floats and depth are scaled by the image's range, written in the name.
    const bool profundidad = e.formato == DkImageFormat_Z24S8;
    const bool flotante = e.formato == DkImageFormat_R32_Float || e.formato == DkImageFormat_RG16_Float ||
                          e.formato == DkImageFormat_RGBA16_Float || e.formato == DkImageFormat_RG32_Float;
    float minimo = 0.0f, maximo = 1.0f;
    if (profundidad || flotante) {
      minimo = profundidad ? c[0] : 0.0f;
      maximo = profundidad ? c[0] : 1.0f;
      for (size_t i = 0; i < texels; ++i) {
        for (int k = 0; k < (profundidad ? 1 : 3); ++k) {
          const float x = c[i * 4 + k];
          if (std::isfinite(x)) {
            minimo = std::min(minimo, x);
            maximo = std::max(maximo, x);
          }
        }
      }
      if (profundidad) {
        for (size_t i = 0; i < texels; ++i) c[i * 4 + 1] = c[i * 4 + 2] = c[i * 4];
      }
    }
    const float escala = maximo > minimo ? 1.0f / (maximo - minimo) : 1.0f;
    std::vector<uint8_t> bgr(texels * 3), a(texels * 3);
    for (size_t i = 0; i < texels; ++i) {
      for (int k = 0; k < 3; ++k) {
        const float x = std::clamp((c[i * 4 + k] - minimo) * escala, 0.0f, 1.0f);
        bgr[i * 3 + (2 - k)] = uint8_t(x * 255.0f + 0.5f);
      }
      const uint8_t al = uint8_t(std::clamp(c[i * 4 + 3], 0.0f, 1.0f) * 255.0f + 0.5f);
      a[i * 3] = a[i * 3 + 1] = a[i * 3 + 2] = al;
    }
    std::snprintf(nombre, sizeof(nombre), "%02u_%08X_%ux%u_g%u%s_rango_%g_%g", unsigned(n), e.base, e.ancho, e.alto,
                  e.formato_guest, profundidad ? "_z" : "", double(minimo), double(maximo));
    EscribirBmp(carpeta / (std::string(nombre) + ".bmp"), e.ancho, e.alto, bgr);
    if (alfa) {
      EscribirBmp(carpeta / (std::string(nombre) + "_alfa.bmp"), e.ancho, e.alto, a);
    }
  }
  if (raw) manifest << "]}";
  REXLOG_INFO("[deko3d] volcado: {} texturas escritas en {}", entradas.size(), carpeta.string());
  });
  volcado_.clear();
}


// ---- Background writes to the SD card (EncolarEscrituraSd)

void EncolarEscrituraSd(std::function<void()> escritura) {
  struct Escritor {
    std::mutex candado;
    std::condition_variable hay;
    std::deque<std::function<void()>> cola;
    std::unique_ptr<rex::thread::Thread> hilo;
  };
  static Escritor* e = new Escritor();  // never destroyed: its thread lives as long as the process
  {
    std::lock_guard<std::mutex> l(e->candado);
    e->cola.push_back(std::move(escritura));
    if (!e->hilo) {
      e->hilo = rex::thread::Thread::Create({}, [] {
        for (;;) {
          std::function<void()> tarea;
          {
            std::unique_lock<std::mutex> l(e->candado);
            e->hay.wait(l, [] { return !e->cola.empty(); });
            tarea = std::move(e->cola.front());
            e->cola.pop_front();
          }
          tarea();
        }
      });
      if (!e->hilo) {
        // No thread: done here, as before.
        auto tarea = std::move(e->cola.back());
        e->cola.pop_back();
        tarea();
        return;
      }
    }
  }
  e->hay.notify_one();
}

}  // namespace rex::x360
