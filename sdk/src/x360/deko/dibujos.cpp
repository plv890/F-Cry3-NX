// nfsmw - deko3d renderer: each draw of the ring. See nfsmw_dk_render.h.
//
// The decisions are the Vulkan backend's (nfsmw_nativo_dibujos.cpp, Dibujar), ported as they are and in the same
// order: what can be skipped, the depth bias, the indices and the vertex ranges, the textures of each sampler, the
// constants, the framing and the specialization bits. The recording is deko3d's own: no pipeline is looked up or
// created. The shaders are bound when they change, each block of state (rasterizer, blending, color mask, depth and
// stencil, vertex input, viewport, scissor) is emitted only when it differs from what this command list last got,
// textures are bound as handles, and the three constant blocks are uniform buffers in the upload ring.
//
// Shaders: one DKSH per (library shader, specialization value masked to the bits it reads, and for a vertex shader
// the interpolators its pixel shader reads), from the DKSH cache (rex/ui/deko3d/cache_dksh.h). A vertex shader variant
// that is not there yet draws with the one that keeps every interpolator, while the exact one compiles on the
// worker; anything else missing is compiled on the spot and added to the cache, which is saved on the SD card.

#include <unordered_set>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <rex/ui/deko3d/compilador.h>
#include <rex/ui/deko3d/glsl.h>
#include <rex/x360/deko/render.h>
#include <rex/x360/shader_entry.h>
#include <rex/x360/xenos.h>
// The glow's bright pass variants (nfsmw_resplandor_cielo), made from the game's shader by tools/switch/dk_resplandor.py
// and not distributed. Without them the setting keeps the game's shader, as in the Vulkan build.

// The same settings as the Vulkan build's (nfsmw_nativo_dibujos.cpp): names, types and defaults.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_inv_tamano_tex, true, "NFSMW",
                    "Renderizador nativo (18/09, build 164): los shaders toman 1/tamano de textura de las "
                    "constantes en vez de preguntarselo a la textura en cada muestreo con desplazamiento")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pcf_barato, true, "NFSMW",
                    "Renderizador nativo (20/09): un solo muestreo del mapa de sombras en vez del patron 3x3 a medio "
                    "texel")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_sesgo_pendiente, 20, "NFSMW",
                     "Renderizador nativo (25/09, build 181): desplazamiento de profundidad por pendiente en el mapa de "
                     "sombras, en decimas (20 = 2,0). Quita el acne. Se cambia en marcha")
    .range(0, 200);
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_sesgo_constante, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 181): desplazamiento de profundidad constante en el mapa de "
                     "sombras, en miles de unidades de 24 bits (1 = 1000). 0 = sin constante. Se cambia en marcha")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(nfsmw_dk_sombras_sesgo_pitch, 1600, "NFSMW",
                     "Ancho minimo de un destino solo de profundidad para darle el sesgo de sombras de arriba cuando el "
                     "juego no pone ninguno (1600 = el mapa de NFS; Dead Space 2 usa 960)")
    .range(64, 8192);
// fc3-switch 2026-11-07 (EXPERIMENT, default off = old behaviour), ported from VasilkovND/gears2-nx (GPL-3.0):
//  - fc3_dk_indices_alineados (their 0047): 16-bit indices with a whole-word byte order (k8in32/k16in32) are paired
//    by the memory's 4-byte-aligned words, not counted from the buffer start; with a start at 2 mod 4 the old code
//    took every index from the other half of its word (triangles across the mesh).
//  - fc3_dk_stride0 (their 0034): a vertex stream with stride 0 (every vertex reads the same element) is bound with
//    stride 0 and drawn instead of rejecting the draw (cause 25).
// fc3-switch 2026-11-08 (EXPERIMENT, default off): fc3_dk_mitades_8in32. A vertex element with 16-bit components
// (k_16_16, k_16_16_16_16 and their FLOAT forms) in a stream whose fetch constant says k8in32: the 32-bit word swap
// leaves the element's FIRST half (X, first in memory) in the word's HIGH 16 bits, but deko3d attributes and dsVfetch
// take X from the low 16 bits, so X<->Y and Z<->W come out swapped. Evidence: VS 1051's per-object record (slot 93,
// endian 2) holds a 3x3 rotation as halves at offsets 12/16/24; read in memory order it is an exact rotation about Z
// ((-0.694,-0.719,0),(0.719,-0.694,0),(0,0,1)), read as now the third row is (0,0,0) -> objects flattened into the
// giant flat triangles. The 16-bit index path already takes the high half first for these words. Fixed by composing
// the swap into the element's g_InputRemap code (no shader change).
REXCVAR_DEFINE_BOOL(fc3_dk_mitades_8in32, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: 16-bit vertex components in k8in32 words: X from the high half (swap X/Y, Z/W)");
// M1 run (2026-11-08): fc3_dk_mitades_8in32 on ALL k8in32 16-bit elements exploded almost every mesh -> the low-half
// decode is right for ordinary streams. fc3_dk_mitades_manual narrows the swap to the index-computed fetches only
// (dsVfetch; VS 1051's per-object matrix), where the memory-order reading gives an exact rotation.
// fc3-switch 2026-11-08 (FIX candidate, default off): BLENDINDICES is a uint4 input in the translated shaders, so a
// NORMALIZED Xenos fetch of it (num_format fraction, e.g. k_8_8_8_8) reached the shader as 0..255 instead of 0..1.
// FC3's vegetation VS (1051 palms and its siblings) pack a direction there and use it as r * 2 - 1: offsets 255x too
// big -> giant leaf cards (the wedges, and the "snow" of the near-camera dither on them). Needs the library variant
// from scripts/fc3_blendindices_scale.py (float4(iBlendIndices0) * shared dword 123); this writes that scale.
REXCVAR_DEFINE_BOOL(fc3_dk_blendindices_normalizados, false, "fc3 enhancements",
                    "deko3d FIX: scale normalized BLENDINDICES fetches to 0..1 (needs the blendindices library)");
REXCVAR_DEFINE_BOOL(fc3_dk_fetch_misma_posicion, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: match each vertex element to the patched fetch at its own instruction position");
REXCVAR_DEFINE_BOOL(fc3_dk_mitades_manual, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: like fc3_dk_mitades_8in32 but only for the shader's own fetches (dsVfetch)");
// fc3-switch 2026-11-09 DIAGNOSTIC: logs the texture fetch constants of one PS's samplers ([pstex]), every 40 frames,
// 30 times (-1 = off).
REXCVAR_DEFINE_INT32(fc3_log_tex_ps, -1, "fc3 enhancements", "DIAGNOSTIC: PS whose sampler fetch constants are logged");
REXCVAR_DEFINE_INT32(fc3_volcado_al_ps, -1, "fc3 enhancements",
                     "DIAGNOSTIC: request a frame dump when this PS (library number) is drawn (3 at most)");
REXCVAR_DEFINE_INT32(fc3_volcado_al_ps_desde, 0, "fc3 enhancements",
                     "DIAGNOSTIC: fc3_volcado_al_ps only from this frame on");
REXCVAR_DEFINE_INT32(fc3_volcado_al_ps_cada, 30, "fc3 enhancements",
                     "DIAGNOSTIC: frames between the dumps of fc3_volcado_al_ps");
REXCVAR_DEFINE_BOOL(fc3_dk_vertices_sin_cache, false, "fc3 enhancements",
                    "deko3d DIAGNOSTIC: every draw copies its vertex ranges again (no same-slot dedupe, no geometry store)");

namespace {
// fc3_dk_mitades_8in32: the remap code (3 bits per output component: 0-3 a source component, 4 zero, 5 one, 7 the
// same component) composed with the X<->Y (and Z<->W for 4 components) swap of the decoded value.
uint32_t RemapeoMitades(uint32_t codigo, uint32_t componentes) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t s = (codigo >> (i * 3)) & 7;
    if (s == 7) {
      s = i;
    }
    if (s < 4 && (s < 2 || componentes == 4)) {
      s ^= 1;
    }
    r |= s << (i * 3);
  }
  return r;
}
}  // namespace
REXCVAR_DEFINE_INT32(fc3_dk_medio_pixel, 0, "fc3 enhancements",
                     "DIAGNOSTIC: D3D9 half-pixel vertex shift: 0 as before, 1 none, 2 opposite sign");
REXCVAR_DEFINE_INT32(fc3_dk_medio_pixel_alternar_s, 0, "fc3 enhancements",
                     "DIAGNOSTIC: from this second on, fc3_dk_medio_pixel and 0 alternate every 20 s (0 = off)");
// fc3-switch 2026-11-12 DIAGNOSTIC (default ""): fc3_dk_color_falso_ps = "n1,n2,..." (or "*" = every PS of the
// 1280-wide passes): those draws output a flat palette colour of their PS number instead of their result, in any pass
// (not only the G-buffer like fc3_dk_color_falso). One screenshot then shows which PS covers which screen area.
REXCVAR_DEFINE_STRING(fc3_dk_color_falso_ps, "", "fc3 enhancements",
                      "DIAGNOSTIC: listed PS numbers (or *) output a palette colour of their number");
REXCVAR_DEFINE_BOOL(fc3_dk_color_falso, false, "fc3 enhancements",
                    "deko3d DIAGNOSTIC: G-buffer (pitch 880) draws write R = VS & 255, G = VS >> 8, B = PS & 255 (/255)");
// fc3-switch 2026-11-11 HACK (AO specks): draws whose PS library number is listed ignore the stencil test. FC3's AO
// (PS 1937, VS 2131) is stencil-masked by a mask that flickers frame to frame on deko3d (cause not found, see
// HANDOFF 2026-11-11); without the test the AO is computed for every pixel of the pass and is stable. "" = off.
REXCVAR_DEFINE_STRING(fc3_dk_sin_estencil_ps, "", "fc3 enhancements",
                      "deko3d HACK: comma-separated PS library numbers whose draws skip the stencil test");
REXCVAR_DEFINE_STRING(fc3_dk_saltar_ps, "", "fc3 enhancements",
                      "deko3d DIAGNOSTIC: comma-separated PS library numbers whose draws are skipped (bisection)");
// fc3-switch 2026-11-11 DIAGNOSTIC (AO specks, test BA1): draws whose PS library number is listed are fenced by a
// full barrier with image/shader/descriptor/L2 invalidation before the draw and a full barrier after it, to test
// whether the 255 warps of PS 1937 come from GPU work running concurrently with that draw. "" = off.
REXCVAR_DEFINE_STRING(fc3_dk_barrera_ps, "", "fc3 enhancements",
                      "deko3d DIAGNOSTIC: comma-separated PS library numbers whose draws get full barriers around them");
// fc3-switch 2026-11-11 DIAGNOSTIC (AO specks, test T2, docs/AO-UAM-RESEARCH-20261111.md): the listed PS library
// numbers are compiled by uam at O3 without its post-RA dual-issue reordering pass, under a separate DKSH cache key.
REXCVAR_DEFINE_STRING(fc3_dk_uam_sin_postra_ps, "", "fc3 enhancements",
                      "deko3d DIAGNOSTIC: comma-separated PS library numbers compiled without uam PostRADualIssue");
REXCVAR_DEFINE_STRING(fc3_dk_saltar_vs, "", "fc3 enhancements",
                      "deko3d DIAGNOSTIC: comma-separated VS library numbers whose draws are skipped (bisection)");
REXCVAR_DEFINE_BOOL(fc3_dk_indices_alineados, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: 16-bit indices in 32-bit-swapped words paired by aligned memory words");
REXCVAR_DEFINE_BOOL(fc3_dk_stride0, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: draw vertex streams with stride 0 instead of rejecting them");
REXCVAR_DEFINE_INT32(fc3_light_test, 0, "NFSMW", "Diagnostic: HDR 540 stencil ALWAYS (1), depth ALWAYS (2), both (3).").range(0, 3);

REXCVAR_DEFINE_BOOL(fc3_dk_sesgo_exp_color, true, "NFSMW",
                    "deko3d (fc3-switch, PROTOTYPE): RB_COLOR_INFO color_exp_bias (the Xenos scales what the pixel shader "
                    "writes to that target by 2^bias) done with the blend constant when the source factor is ONE");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_invisibles, true, "NFSMW",
                    "Renderizador nativo (20/09): saltarse los dibujos que no pueden cambiar ni un pixel (mezcla que "
                    "copia el destino, o prueba de alfa con la funcion NUNCA)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sin_desenfoque, true, "NFSMW",
                    "Quitar el desenfoque radial de la composicion final. Son 7 de los 12 muestreos del "
                    "unico cuadrilatero a pantalla completa del posproceso (1,5-2,0 ms reales). Se pierde "
                    "el difuminado de los bordes al acelerar y con el NOS: la imagen queda mas nitida");
REXCVAR_DEFINE_INT32(nfsmw_dk_envio_parcial, 256, "NFSMW",
                     "deko3d: cada N dibujos lo grabado del fotograma se envia ya a la GPU (Nucleo::EnviarParcial), para "
                     "que la GPU dibuje mientras la CPU sigue grabando. 0 = un solo envio por fotograma, como antes "
                     "(Dead Space 3 en peleas: la GPU parada 2-10 ms por fotograma esperando a que se cerrara el "
                     "trabajo; gears2-nx 0039)")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(nfsmw_dk_sesgo_igualdad, true, "NFSMW",
                    "deko3d: los dibujos de color que prueban la profundidad con GEQUAL o LEQUAL sin sesgo propio se "
                    "acercan 4 unidades: pasan donde un pase previo de solo profundidad escribio la misma superficie "
                    "con otro vertex shader (Gears of War 2: sin esto la escena salia negra)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_z_temprana, true, "NFSMW",
                    "Renderizador nativo (20/09): en los dibujos que prueban la profundidad pero NO la "
                    "escriben (humo, particulas, cristales, calcomanias), declarar EarlyFragmentTests en el "
                    "pixel shader para que la GPU pruebe la profundidad ANTES de sombrear en vez de despues");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cielo_aplazado, true, "NFSMW",
                    "Renderizador nativo (22/09): aplazar el dibujo del cielo hasta despues de los opacos "
                    "del mismo pase, en vez de dibujarlo el primero sobre un Z-buffer vacio. Es un solo "
                    "dibujo opaco que prueba la profundidad y no la escribe, asi que la imagen es identica");
REXCVAR_DEFINE_BOOL(nfsmw_dk_constantes_por_diferencias, true, "NFSMW",
                    "deko3d: las constantes de los shaders se mandan por diferencias (solo los registros que cambiaron, "
                    "dentro de la lista de comandos) en vez de copiar el bloque entero en cada dibujo. false = copiar")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sin_ps_sin_color, true, "NFSMW",
                    "Renderizador nativo (17/09, build 157): en los dibujos que no escriben ningun color, sin etapa de "
                    "fragmentos si el pixel shader no puede descartar pixeles");
// Defined in nfsmw_dk_destinos.cpp (the game hooks read them too).
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);
REXCVAR_DECLARE(bool, nfsmw_dk_perfil_barrera);
REXCVAR_DECLARE(bool, nfsmw_dk_perfil_variantes);
REXCVAR_DECLARE(bool, nfsmw_nativo_ps_solo_alfa);
REXCVAR_DECLARE(bool, fc3_trace_lighting_checkpoint);
REXCVAR_DECLARE(bool, fc3_dk_copiar_alto_usado);  // destinos.cpp (N13)
// deko3d only.
REXCVAR_DEFINE_BOOL(nfsmw_dk_compilar_en_fondo, true, "NFSMW",
                    "deko3d: la variante exacta de un vertex shader que no esta en la cache se compila en segundo "
                    "plano; mientras, dibuja la que conserva todos los interpoladores. false = se compila en el acto")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_dk_nunca_en_el_acto, true, "NFSMW",
                    "deko3d (fc3-switch): un shader que no esta en la cache se compila siempre en segundo plano y sus "
                    "dibujos se omiten mientras tanto (uam puede tardar minutos en uno). false = en el acto")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_terrain_psize_experiment, false, "FC3",
                    "Bind the terrain VS PSIZE input at location 2; requires the matching experimental shader library")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_extra_vertex_semantics, false, "FC3",
                    "Bind PSIZE and FOG vertex inputs in free locations; requires the matching shader library")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_biased_texture_experiment, false, "FC3",
                    "Pass unconverted BC1 biased-channel masks to the experimental material shader")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// fc3-switch 2026-11-07 (EXPERIMENT/HACK, default off = old behaviour): vertex fetches read at index 0.
// 13 FC3 vertex shaders overwrite r0.x (the Xenos vertex index) with r0 = (-abs(r0.x) > c), c = 0 (literal or def),
// i.e. 0, and only then fetch some TEXCOORDs from r0.x: per-object data (one 16-32 byte record per draw, the fetch
// constant's size is the rest of its 4 KB page). The translator (XenosRecomp) marks only fetches whose source is not
// r0.x as manual, so these became per-vertex attributes; the vertex range (indices up to ~1365) then lies past the
// 480-752 byte buffer and the whole draw was rejected (cause 12, ~2500-4800 draws per run, VS 324/249). With this cvar
// a stream whose every element is such a fetch is bound with stride 0 from the fetch base (every vertex reads record
// 0, as the shader does). HACK: the shader list is a table (scripts/fc3_vs_r0_overwrite_scan.py over the translated
// HLSL, key = container XXH3); the proper fix is the translator marking these fetches manual.
REXCVAR_DEFINE_INT32(fc3_dk_mitades_manual_alternar_s, 0, "fc3 enhancements",
                     "DIAGNOSTIC: from this second on, fc3_dk_mitades_manual is on/off every 10 s (0 = always as set)");
REXCVAR_DEFINE_BOOL(fc3_dk_vfetch_mini, false, "fc3 enhancements",
                    "vfetch_mini inherits the fetch constant and stride of the previous vfetch_full (grass streaks)");
REXCVAR_DEFINE_BOOL(fc3_dk_grass_fetch_write_mask, false, "fc3 enhancements",
                    "Match the two reordered grass r4 fetches by destination write mask")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_dk_grass_instance_blendindices, false, "fc3 enhancements",
                    "Test reading the grass VS BLENDINDICES from its per-instance record")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// fc3-switch 2026-11-12 EXPERIMENT (GW2, default off): VS 1632 (grass, container AF5781B94E1A5994) fills r4 from two
// manual fetches: instruction 33 (k9, BLENDINDICES0, writes r4.zw) and 35 (k11, TEXCOORD6, writes r4.xy). Hypothesis:
// both halves belong to the per-instance record (slot 94, stride 32): r4.xy = 8_8_8_8 @16, r4.zw = 16_16 @20. With
// this cvar k9 is bound to 94 / fmt 25 / @20 (signed, remap identity) and k11 to 94 / fmt 6 / @16 (remap FCA, as the
// patched XY fetch logged in GW1b). HACK: fixed descriptors for one shader, not a general fetch-matching fix.
// fc3-switch 2026-11-12 EXPERIMENT (GW3, default off): fc3_dk_fetch_mas_cercano. D3D's patched vertex program may
// reorder fetches among the element positions (VS 1632: container 33/34/35 = r4.zw/r8/r4.xy, patched 33/34/35 =
// r8/r4.xy/r4.zw). The old search took the FIRST element fetch writing the same register (VS 1632 k9/k11 -> the mesh
// COLOR0 fetch at instruction 13, slot 95). With this cvar: among the patched fetches at element positions, the one
// with the same destination register and an intersecting write mask NEAREST to the element's own position (ties:
// identical mask). Changes against the old choice, and matches more than 2 instructions away, are logged ([fetchsel]).
REXCVAR_DECLARE(bool, fc3_sonda);
REXCVAR_DEFINE_BOOL(fc3_dk_fetch_mas_cercano, false, "fc3 enhancements",
                    "EXPERIMENT: match a vertex element to the nearest patched fetch with the same register/write mask")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_dk_grass_record_pair, false, "fc3 enhancements",
                    "EXPERIMENT: bind the grass VS r4 halves (instr 33/35) to the per-instance record (94 @20 / @16)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_dk_fetch_indice_cero, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: bind vertex streams the VS reads at index 0 (r0.x overwritten) with stride 0")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::x360 {
namespace {

// Container XXH3 -> TEXCOORD indices (bit n = TEXCOORDn) fetched after r0 = -abs(r0.x) > 0 (see fc3_dk_fetch_indice_cero).
struct VsIndiceCero {
  uint64_t huella_contenedor;
  uint16_t texcoords;
};
constexpr VsIndiceCero kVsIndiceCero[] = {
    {0x18572edcab7065cfull, 0x3C}, {0x1a4aba532ddd54aeull, 0x20}, {0x21039ea7f0390060ull, 0x3C},
    {0x232f56c8376d7828ull, 0x04}, {0x283c16f9a0901071ull, 0x20}, {0x5e529a48f4605127ull, 0x3C},
    {0x6f3573c20c800c00ull, 0x3C}, {0x81a7397d50be98dcull, 0x3C}, {0x8426a7c4c971624full, 0x04},
    {0x94ef2621806d53b1ull, 0x24}, {0xa039ae10b7a763baull, 0x3C}, {0xaacb27860a3441b9ull, 0x20},
    {0xfad03e519da281aeull, 0x3C},
};
uint32_t TexcoordsIndiceCero(uint64_t huella_contenedor) {
  for (const VsIndiceCero& v : kVsIndiceCero) {
    if (v.huella_contenedor == huella_contenedor) return v.texcoords;
  }
  return 0;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kSpecInvTamanoTex = uint32_t(1) << 9;
constexpr uint32_t kSpecPcfBarato = uint32_t(1) << 15;
constexpr uint32_t kSpecFuncionAlfaDesplazamiento = 16;
// SPEC_CONSTANT_SIN_DESENFOQUE (shader_common.h): the final composite without its radial blur.
constexpr uint32_t kSpecSinDesenfoque = uint32_t(1) << 19;
// The containers the Vulkan backend singles out by fingerprint: the race's final composite (p_000139) and the
// glow's bright pass (p_000094).
constexpr uint64_t kHuellaComposicion = 0x19C0C358044A29BFull;
constexpr uint64_t kHuellaBrightPass = 0xE849A9F6D3323B87ull;
// Pixel shader variants (ShaderDe's variante_ps, in the DKSH cache key): the GLSL with
// layout(early_fragment_tests) (the Vulkan path's kSpecZTemprana, which patches the SPIR-V), and the bright pass
// replaced by its natural or soft variant.
// nfsmw_nativo_cielo_aplazado: the sky's pixel shader (the only one whose constant table names CloudIntensity and
// SkyAlphaTag), the dome's index count (160 triangles) and the frames the guard watches.
constexpr uint64_t kHuellaCielo = 0x28AA3CDAC6C19705ull;
constexpr uint32_t kIndicesDomoCielo = 480;
constexpr uint64_t kCieloFotogramasPrueba = 90;
// SPEC_CONSTANT_SOMBRA_MINIMO (shader_common.h): tfetch2DSombraMin takes the minimum with its pair (binding 16 + R).
constexpr uint32_t kSpecSombraMinimo = uint32_t(1) << 23;
constexpr uint32_t kLadoSombrasDibujos = 1600;  // a depth-only target this wide is the shadow map
constexpr uint16_t kSwizzleParejaSombra = 0;    // kSwizzleRRRR (nfsmw_dk_texturas.cpp): depth in every channel
constexpr uint32_t kVarianteZTemprana = 1;
constexpr uint32_t kVarianteResplandorNatural = 2;
constexpr uint32_t kVarianteResplandorSuave = 4;
constexpr uint32_t kPalabraInvTamano = 90;
constexpr uint32_t kMaxVerticesPorDibujo = uint32_t(1) << 20;
constexpr uint32_t kUboBytesVs = 256 * 16;
constexpr uint32_t kUboBytesPs = 224 * 16;
constexpr uint32_t kUboBytesCompartidas = 32 * 16;
constexpr uint32_t kTodasLasSalidas = ~0u;

// The 16-byte registers of [desde, hasta) (bytes) that differ from `sombra` are pushed into the block at `bloque` and
// copied to `sombra`; runs of changed registers go in one push. Returns the registers pushed.
uint32_t EmpujarDiferencias(DkCmdBuf cmd, DkGpuAddr bloque, uint32_t tam_bloque, const uint32_t* actual,
                            uint32_t* sombra, uint32_t desde, uint32_t hasta, uint64_t& empujes) {
  auto iguales = [&](uint32_t r) {
    uint64_t a[2], b[2];
    std::memcpy(a, actual + r * 4, 16);
    std::memcpy(b, sombra + r * 4, 16);
    return a[0] == b[0] && a[1] == b[1];
  };
  uint32_t empujados = 0;
  const uint32_t fin = hasta / 16;
  for (uint32_t r = desde / 16; r < fin;) {
#if defined(__aarch64__)
    // Four registers (64 bytes) at a time while they are all equal: most of the block does not change between draws.
    while (r + 4 <= fin) {
      const uint8_t* a = reinterpret_cast<const uint8_t*>(actual + r * 4);
      const uint8_t* b = reinterpret_cast<const uint8_t*>(sombra + r * 4);
      const uint8x16_t d = vorrq_u8(vorrq_u8(veorq_u8(vld1q_u8(a), vld1q_u8(b)), veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16))),
                                    vorrq_u8(veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32)), veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48))));
      if (vmaxvq_u8(d) != 0) {
        break;
      }
      r += 4;
    }
    if (r >= fin) {
      break;
    }
#endif
    if (iguales(r)) {
      ++r;
      continue;
    }
    uint32_t f = r + 1;
    while (f < fin && !iguales(f)) {
      ++f;
    }
    const uint32_t bytes = (f - r) * 16;
    dkCmdBufPushConstants(cmd, bloque, tam_bloque, r * 16, bytes, actual + r * 4);
    std::memcpy(sombra + r * 4, actual + r * 4, bytes);
    empujados += f - r;
    ++empujes;
    r = f;
  }
  return empujados;
}

// The per-fetch cache's slot: a multiply-xor mix of the six words of a texture fetch constant (it was an XXH3 call per
// sampler per draw; the slot is checked against the whole words anyway).
inline uint64_t ClaveFetch(const uint32_t* f) {
  uint64_t a, b, c;
  std::memcpy(&a, f, 8);
  std::memcpy(&b, f + 2, 8);
  std::memcpy(&c, f + 4, 8);
  uint64_t h = (a ^ 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
  h = (h ^ (h >> 31) ^ b) * 0x94D049BB133111EBull;
  h = (h ^ (h >> 29) ^ c) * 0xBF58476D1CE4E5B9ull;
  return h ^ (h >> 32);
}

// layout(early_fragment_tests) in the GLSL of a pixel shader: before its first declaration, after every preprocessor
// line (SPIRV-Cross puts an #extension under #version, and no declaration may come before an #extension).
void DeclararZTemprana(std::string& glsl) {
  size_t pos = glsl.find("\nlayout(");
  if (pos == std::string::npos) {
    pos = glsl.find("\nvoid main(");
  }
  glsl.insert(pos == std::string::npos ? glsl.size() : pos + 1, "layout(early_fragment_tests) in;\n");
}

// The 16-bit indices of a draw with NEON, 16 per iteration (the Vulkan backend's IndicesDe16Neon, checked there
// against the plain loop). With kEscribir false only their minimum and maximum (the draw's vertex range, before the
// upload space is known); with true they are also written to `salida`, the store or the ring (CPU-uncached: written
// once, in order).
template <bool kEscribir>
void IndicesDe16(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                 uint32_t& maximo) {
  uint32_t i = 0;
  uint32_t mn = 0xFFFF;
  uint32_t mx = 0;
#if defined(__aarch64__)
  if (cuenta >= 16) {
    uint16x8_t min_a = vdupq_n_u16(0xFFFF);
    uint16x8_t min_b = min_a;
    uint16x8_t max_a = vdupq_n_u16(0);
    uint16x8_t max_b = max_a;
    if (girar) {
      for (; i + 16 <= cuenta; i += 16) {
        const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(datos + size_t(i) * 2)));
        const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(datos + size_t(i) * 2 + 16)));
        if constexpr (kEscribir) {
          vst1q_u16(salida + i, a);
          vst1q_u16(salida + i + 8, b);
        }
        min_a = vminq_u16(min_a, a);
        min_b = vminq_u16(min_b, b);
        max_a = vmaxq_u16(max_a, a);
        max_b = vmaxq_u16(max_b, b);
      }
    } else {
      for (; i + 16 <= cuenta; i += 16) {
        const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(datos + size_t(i) * 2));
        const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(datos + size_t(i) * 2 + 16));
        if constexpr (kEscribir) {
          vst1q_u16(salida + i, a);
          vst1q_u16(salida + i + 8, b);
        }
        min_a = vminq_u16(min_a, a);
        min_b = vminq_u16(min_b, b);
        max_a = vmaxq_u16(max_a, a);
        max_b = vmaxq_u16(max_b, b);
      }
    }
    mn = vminvq_u16(vminq_u16(min_a, min_b));
    mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
  }
#endif
  for (; i < cuenta; ++i) {
    uint16_t v;
    std::memcpy(&v, datos + size_t(i) * 2, 2);
    if (girar) {
      v = std::byteswap(v);
    }
    if constexpr (kEscribir) {
      salida[i] = v;
    }
    mn = std::min<uint32_t>(mn, v);
    mx = std::max<uint32_t>(mx, v);
  }
  minimo = mn;
  maximo = mx;
}

// The other index cases as 32 bits: 32-bit indices (24 significant bits), any byte order, the restart index (written
// as 0xFFFFFFFF, deko3d's restart value) and the index offset (VGT_INDX_OFFSET). minimo > maximo: restarts only.
template <bool kEscribir>
void IndicesDe32(const uint8_t* datos, uint32_t cuenta, bool indices32, xenos::Endian orden, bool reinicio,
                 uint32_t indice_reinicio, uint32_t desplazamiento, uint32_t* salida, uint32_t& minimo,
                 uint32_t& maximo) {
  uint32_t mn = UINT32_MAX, mx = 0;
  const bool alineados = REXCVAR_GET(fc3_dk_indices_alineados);
  for (uint32_t i = 0; i < cuenta; ++i) {
    uint32_t v;
    if (indices32) {
      uint32_t crudo;
      std::memcpy(&crudo, datos + size_t(i) * 4, 4);
      v = xenos::GpuSwap(crudo, orden) & 0xFFFFFF;
    } else {
      // The GPU reads 16-bit indices as 32-bit words and swaps the whole word: with k8in32 or k16in32 the two indices
      // of a word trade places (GpuSwap of a single uint16_t only knows kNone and k8in16, and returned the others
      // unswapped: indices like 0x0100 for 1, triangles across a whole mesh). Each index is the matching half of its
      // swapped word; the same result as before for kNone and k8in16.
      uint32_t palabra = 0;
      const uint8_t* par = datos + size_t(i & ~1u) * 2;
      bool alta = (i & 1) != 0;
      if (alineados) {
        // fc3_dk_indices_alineados (gears2-nx 0047): the words are the memory's, 4-byte aligned (host and guest
        // addresses agree mod 4: guest memory is mapped page-aligned).
        const uint8_t* indice = datos + size_t(i) * 2;
        par = reinterpret_cast<const uint8_t*>(uintptr_t(indice) & ~uintptr_t(3));
        alta = (uintptr_t(indice) & 2) != 0;
      }
      // The last index of an odd count has no partner: its word's other half is padding the GPU reads too, unless
      // it would cross into the next page.
      std::memcpy(&palabra, par, (i + 1 == cuenta && !alta && (uintptr_t(par) & 0xFFF) > 0xFFC) ? 2 : 4);
      palabra = xenos::GpuSwap(palabra, orden);
      v = alta ? (palabra >> 16) : (palabra & 0xFFFF);
    }
    if (reinicio && v == indice_reinicio) {
      if constexpr (kEscribir) {
        salida[i] = UINT32_MAX;
      }
      continue;
    }
    v = (v + desplazamiento) & 0xFFFFFF;
    mn = std::min(mn, v);
    mx = std::max(mx, v);
    if constexpr (kEscribir) {
      salida[i] = v;
    }
  }
  minimo = mn;
  maximo = mx;
}

// The Xbox 360 vertex format as deko3d's attribute size and type (FormatoAtributo in the Vulkan backend). false if
// it is not supported.
bool AtributoDe(uint32_t formato, bool entrada_entera, bool con_signo, bool entero, DkVtxAttribSize& tamano,
                DkVtxAttribType& tipo, bool& r11g11b10) {
  r11g11b10 = false;
  const DkVtxAttribType normal = entrada_entera ? (con_signo ? DkVtxAttribType_Sint : DkVtxAttribType_Uint)
                                 : entero       ? (con_signo ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled)
                                                : (con_signo ? DkVtxAttribType_Snorm : DkVtxAttribType_Unorm);
  const DkVtxAttribType coma = entrada_entera ? DkVtxAttribType_Uint : DkVtxAttribType_Float;
  switch (formato) {
    case 6:  // k_8_8_8_8
      tamano = DkVtxAttribSize_4x8;
      tipo = normal;
      return true;
    case 7:  // k_2_10_10_10
      tamano = DkVtxAttribSize_10_10_10_2;
      tipo = normal;
      return true;
    case 16:  // k_10_11_11: a packed normal the shader decodes itself
      if (!entrada_entera) {
        return false;
      }
      r11g11b10 = true;
      tamano = DkVtxAttribSize_1x32;
      tipo = DkVtxAttribType_Uint;
      return true;
    case 25:  // k_16_16
      tamano = DkVtxAttribSize_2x16;
      tipo = normal;
      return true;
    case 26:  // k_16_16_16_16
      tamano = DkVtxAttribSize_4x16;
      tipo = normal;
      return true;
    case 31:  // k_16_16_FLOAT
    case 32:  // k_16_16_16_16_FLOAT
      if (entrada_entera) {
        return false;
      }
      tamano = formato == 31 ? DkVtxAttribSize_2x16 : DkVtxAttribSize_4x16;
      tipo = DkVtxAttribType_Float;
      return true;
    case 33:  // k_32
    case 34:  // k_32_32
    case 35:  // k_32_32_32_32
      if (!entrada_entera) {
        return false;
      }
      tamano = formato == 33 ? DkVtxAttribSize_1x32 : formato == 34 ? DkVtxAttribSize_2x32 : DkVtxAttribSize_4x32;
      tipo = con_signo ? DkVtxAttribType_Sint : DkVtxAttribType_Uint;
      return true;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      tamano = DkVtxAttribSize_1x32;
      tipo = coma;
      return true;
    case 37:  // k_32_32_FLOAT
      tamano = DkVtxAttribSize_2x32;
      tipo = coma;
      return true;
    case 57:  // k_32_32_32_FLOAT
      tamano = DkVtxAttribSize_3x32;
      tipo = coma;
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      tamano = DkVtxAttribSize_4x32;
      tipo = coma;
      return true;
    default:
      return false;
  }
}

// Xenos blend factor -> deko3d (the Vulkan backend's kFactores).
DkBlendFactor FactorDe(uint32_t f) {
  switch (f) {
    case 1: return DkBlendFactor_One;
    case 4: return DkBlendFactor_SrcColor;
    case 5: return DkBlendFactor_InvSrcColor;
    case 6: return DkBlendFactor_SrcAlpha;
    case 7: return DkBlendFactor_InvSrcAlpha;
    case 8: return DkBlendFactor_DstColor;
    case 9: return DkBlendFactor_InvDstColor;
    case 10: return DkBlendFactor_DstAlpha;
    case 11: return DkBlendFactor_InvDstAlpha;
    case 12: return DkBlendFactor_ConstColor;
    case 13: return DkBlendFactor_InvConstColor;
    case 14: return DkBlendFactor_ConstAlpha;
    case 15: return DkBlendFactor_InvConstAlpha;
    case 16: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_Zero;
  }
}

// Xenos blend op: 0 add, 1 subtract, 2 min, 3 max, 4 reverse subtract (which deko3d has).
DkBlendOp OperacionDe(uint32_t op) {
  switch (op) {
    case 1: return DkBlendOp_Sub;
    case 2: return DkBlendOp_Min;
    case 3: return DkBlendOp_Max;
    case 4: return DkBlendOp_RevSub;
    default: return DkBlendOp_Add;
  }
}

template <class T>
bool Iguales(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

template <class T>
T Ceros() {
  T t;
  std::memset(&t, 0, sizeof(t));
  return t;
}

}  // namespace

// ---- Vertex input (the Vulkan backend's EntradaDe, ClaveEntrada and CalcularEntrada)

uint64_t RenderDk::ClaveEntrada(const PeticionDibujo& p) const {
  const EntradaShader& vs = *p.vs;
  const auto& parcheado = p.vs_microcodigo;
  if (parcheado.size() != vs.microcodigo.size() || vs.elementos.size() > 32) {
    return 0;
  }
  std::array<uint32_t, 32 * 6> palabras;
  size_t n = 0;
  for (const ElementoVertice& elemento : vs.elementos) {
    const size_t i = size_t(elemento.instruccion) * 3;
    if (i + 2 >= parcheado.size()) {
      return 0;
    }
    palabras[n++] = (uint32_t(elemento.instruccion) << 16) | (uint32_t(elemento.uso) << 8) | elemento.indice_uso;
    palabras[n++] = vs.microcodigo[i];
    palabras[n++] = vs.microcodigo[i + 1];
    palabras[n++] = parcheado[i];
    palabras[n++] = parcheado[i + 1];
    palabras[n++] = parcheado[i + 2];
  }
  const uint64_t clave = XXH3_64bits_withSeed(palabras.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
  return clave ? clave : 1;
}

const EntradaVerticesDk* RenderDk::EntradaDe(const PeticionDibujo& p) {
  if (entrada_generacion_ == p.generacion_vs && entrada_vs_ == p.vs) {
    return entrada_actual_;
  }
  entrada_generacion_ = p.generacion_vs;
  entrada_vs_ = p.vs;
  const uint64_t clave = ClaveEntrada(p);
  if (clave) {
    if (const auto it = entradas_.find(clave); it != entradas_.end()) {
      entrada_actual_ = it->second.get();
      return entrada_actual_;
    }
  }
  auto entrada = std::make_unique<EntradaVerticesDk>();
  if (!CalcularEntrada(p, *entrada)) {
    entrada_actual_ = nullptr;
    return nullptr;
  }
  entrada_actual_ = entrada.get();
  if (clave && entradas_.size() < 4096) {
    entradas_.emplace(clave, std::move(entrada));
  } else {
    // Without a key it is kept anyway (the pointer must stay valid while it is the current one).
    entradas_.emplace(uint64_t(uintptr_t(entrada_actual_)) | (uint64_t(1) << 63), std::move(entrada));
  }
  return entrada_actual_;
}

// fc3_dk_mitades_manual, optionally alternating (fc3_dk_mitades_manual_alternar_s: from that second on, on/off every
// 10 s, each switch logged) for an A/B in one run at the same route.
static bool MitadesManualActivas(uint64_t fotograma) {
  const bool base = REXCVAR_GET(fc3_dk_mitades_manual);
  const int32_t desde = REXCVAR_GET(fc3_dk_mitades_manual_alternar_s);
  if (desde <= 0) return base;
  static const auto inicio = std::chrono::steady_clock::now();
  const int64_t s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio).count();
  const bool activo = s >= desde && (((s - desde) / 10) % 2 == 0) && base;
  static int anterior = -1;
  if (int(activo) != anterior) {
    anterior = int(activo);
    REXLOG_INFO("[mitades] manual halves swap {} from second {} (frame {})", activo ? "ON" : "off", s, fotograma);
  }
  return activo;
}

bool RenderDk::CalcularEntrada(const PeticionDibujo& p, EntradaVerticesDk& entrada) {
  const EntradaShader& vs = *p.vs;
  entrada.remapeos.fill(kRemapeoIdentidad);
  const auto& parcheado = p.vs_microcodigo;
  if (parcheado.size() != vs.microcodigo.size()) {
    return Rechazar(20, "el VS del anillo no tiene la longitud del contenedor");
  }
  const uint32_t texcoords_cero =
      REXCVAR_GET(fc3_dk_fetch_indice_cero) ? TexcoordsIndiceCero(vs.huella_contenedor) : 0;
  // Keep this allocation order in sync with scripts/fc3_extra_vertex_semantics.py.
  // Reserve every existing semantic first, then allocate PSIZE and FOG by
  // semantic/index, preferring location 2 so the tested terrain pair retains
  // its input ABI. Manual fetches are also reserved for deterministic layout.
  std::array<std::array<int8_t, 16>, 2> extra_ubicaciones;
  for (auto& ubicaciones : extra_ubicaciones) ubicaciones.fill(-1);
  if (REXCVAR_GET(fc3_extra_vertex_semantics)) {
    uint32_t ocupadas = 0;
    for (const ElementoVertice& e : vs.elementos) {
      const int32_t ubicacion = UbicacionDeUso(e.uso, e.indice_uso);
      if (ubicacion >= 0 && ubicacion < 16) ocupadas |= 1u << ubicacion;
    }
    for (uint32_t clase = 0; clase < 2; ++clase) {
      const uint8_t uso = clase == 0 ? 4 : 11;
      for (uint32_t indice = 0; indice < 16; ++indice) {
        const bool presente = std::any_of(vs.elementos.begin(), vs.elementos.end(), [&](const ElementoVertice& e) {
          return e.uso == uso && e.indice_uso == indice;
        });
        if (!presente) continue;
        int32_t elegida = (ocupadas & (1u << 2)) ? -1 : 2;
        if (elegida < 0) {
          for (int32_t i = 0; i < 16; ++i) {
            if (!(ocupadas & (1u << i))) { elegida = i; break; }
          }
        }
        if (elegida < 0) return Rechazar(22, "sin ubicacion para entrada PSIZE o FOG");
        extra_ubicaciones[clase][indice] = int8_t(elegida);
        ocupadas |= 1u << elegida;
      }
    }
  }
  for (DkVtxAttribState& a : entrada.atributos) {
    a = Ceros<DkVtxAttribState>();
    a.isFixed = 1;  // a location the declaration does not give reads a constant
    a.size = DkVtxAttribSize_4x32;
    a.type = DkVtxAttribType_Float;
  }
  uint32_t ubicaciones_usadas = 0;
  for (const ElementoVertice& elemento : vs.elementos) {
    const uint32_t registro = (vs.microcodigo[size_t(elemento.instruccion) * 3] >> 12) & 0x3F;
    const uint32_t original = vs.microcodigo[size_t(elemento.instruccion) * 3 + 1] & 0xFFF;
    // D3D can reorder the two disjoint writes to r4 in VS 1632. A matching
    // destination register alone is insufficient for these instructions.
    const bool grass_write_mask = REXCVAR_GET(fc3_dk_grass_fetch_write_mask) &&
                                  vs.huella_contenedor == 0xAF5781B94E1A5994ull &&
                                  (elemento.instruccion == 33 || elemento.instruccion == 35);
    const uint32_t mascara_original = MascaraEscrita(original);
    size_t q = SIZE_MAX;
    // fc3-switch 2026-11-08 (EXPERIMENT, default off): fc3_dk_fetch_misma_posicion. The search below matches an element
    // to a patched fetch by destination register and write mask only; when two fetches write the same register with
    // the same mask (VS 1051: TEXCOORD-like PSIZE r7.xyz at instruction 21 and the per-object matrix row r7.xyz at
    // 116) the FIRST one wins, so the matrix row read the mesh stream (slot 95, 8_8_8_8 @36) instead of the record
    // (slot 93 @24) -> degenerate world matrix -> giant flat triangles. The patched microcode has the original's
    // layout (same size; ClaveEntrada already reads it at the element's own position): take that position first.
    if (REXCVAR_GET(fc3_dk_fetch_misma_posicion)) {
      const size_t i = size_t(elemento.instruccion) * 3;
      if (i + 2 < parcheado.size() && ((parcheado[i] >> 12) & 0x3F) == registro && (parcheado[i] & 0x1F) == 0 &&
          (!grass_write_mask || MascaraEscrita(parcheado[i + 1] & 0xFFF) == mascara_original)) {
        q = i;
      }
    }
    for (const ElementoVertice& otro : vs.elementos) {
      if (q != SIZE_MAX && REXCVAR_GET(fc3_dk_fetch_misma_posicion)) {
        break;
      }
      const size_t i = size_t(otro.instruccion) * 3;
      if (((parcheado[i] >> 12) & 0x3F) != registro || (parcheado[i] & 0x1F) != 0) {
        continue;
      }
      if (q == SIZE_MAX) {
        q = i;
      }
      if (MascaraEscrita(parcheado[i + 1] & 0xFFF) == mascara_original) {
        q = i;  // the one that writes the same components
        break;
      }
    }
    if (REXCVAR_GET(fc3_dk_fetch_mas_cercano)) {
      size_t mejor = SIZE_MAX;
      uint32_t mejor_distancia = UINT32_MAX;
      bool mejor_exacta = false;
      for (const ElementoVertice& otro : vs.elementos) {
        const size_t i = size_t(otro.instruccion) * 3;
        if (i + 2 >= parcheado.size() || ((parcheado[i] >> 12) & 0x3F) != registro || (parcheado[i] & 0x1F) != 0) {
          continue;
        }
        const uint32_t mascara = MascaraEscrita(parcheado[i + 1] & 0xFFF);
        if (!(mascara & mascara_original)) {
          continue;
        }
        const uint32_t distancia = otro.instruccion > elemento.instruccion ? otro.instruccion - elemento.instruccion
                                                                           : elemento.instruccion - otro.instruccion;
        const bool exacta = mascara == mascara_original;
        if (distancia < mejor_distancia || (distancia == mejor_distancia && exacta && !mejor_exacta)) {
          mejor = i;
          mejor_distancia = distancia;
          mejor_exacta = exacta;
        }
      }
      if (mejor != SIZE_MAX && (mejor != q || mejor_distancia > 2)) {
        static std::unordered_set<uint64_t> informados;
        const uint64_t clave_informe = (uint64_t(vs.numero) << 16) | elemento.instruccion;
        if (informados.size() < 4096 && informados.insert(clave_informe).second) {
          auto describir = [&](size_t j) {
            if (j == SIZE_MAX) return std::string("none");
            const uint32_t a0 = parcheado[j], a1 = parcheado[j + 1], a2 = parcheado[j + 2];
            return fmt::format("instr {} slot {} fmt {} off {} stride {} swz {:03X}", j / 3,
                               ((a0 >> 20) & 0x1F) * 3 + ((a0 >> 25) & 0x3), (a1 >> 16) & 0x3F,
                               (int32_t(a2 << 1) >> 9) * 4, (a2 & 0xFF) * 4, a1 & 0xFFF);
          };
          REXLOG_INFO("[fetchsel] vs {} instr {} uso {}:{} r{} mask {:X}{}: old {} -> new {} (distance {}{})", vs.numero,
                      elemento.instruccion, elemento.uso, elemento.indice_uso, registro, mascara_original,
                      elemento.manual ? " manual" : "", describir(q), describir(mejor), mejor_distancia,
                      mejor_exacta ? "" : ", partial mask");
        }
      }
      if (mejor != SIZE_MAX) {
        q = mejor;
      }
    }
    if (q == SIZE_MAX) {
      return Rechazar(21, "fetch de vertices no encontrado en el VS parcheado");
    }
    const uint32_t d0 = parcheado[q], d1 = parcheado[q + 1], d2 = parcheado[q + 2];
    uint32_t ranura = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
    const uint32_t formato = (d1 >> 16) & 0x3F;
    uint32_t zancada = (d2 & 0xFF) * 4;
    // fc3-switch 2026-11-09 (fc3_dk_vfetch_mini): a vfetch_mini (dword 1 bit 30) takes its fetch constant and stride
    // from the previous vfetch_full in program order (Shader::GatherVertexFetchInformation); its own fields are not
    // meaningful. FC3's grass VS 1632 reads its per-instance record (slot 94, stride 32) through mini fetches; one of
    // them (k9, @16) was bound to the mesh stream (slot 95, stride 20): the grass came out stretched while walking.
    if (((d1 >> 30) & 0x1) && REXCVAR_GET(fc3_dk_vfetch_mini)) {
      // Search the whole patched microcode (not just vs.elementos) for the previous vfetch_full in program order:
      // D3D's copy path may add instance fetches that are not in the container's vertex declaration, so vs.elementos
      // alone misses them. Shader::GatherVertexFetchInformation uses the same rule (last non-mini fetch before this one).
      size_t mejor = SIZE_MAX;
      for (size_t j = q; j >= 3; ) {
        j -= 3;
        if (j + 2 >= parcheado.size()) continue;
        if ((parcheado[j] & 0x1F) == 0 && !((parcheado[j + 1] >> 30) & 0x1)) {
          mejor = j;
          break;
        }
      }
      static uint32_t informes = 0;
      if (informes < 64) {
        ++informes;
        if (mejor != SIZE_MAX) {
          const uint32_t r0 = ((parcheado[mejor] >> 20) & 0x1F) * 3 + ((parcheado[mejor] >> 25) & 0x3);
          const uint32_t z0 = (parcheado[mejor + 2] & 0xFF) * 4;
          REXLOG_INFO("[vfmini] vs {} element {} (instr {}): mini fetch slot {} stride {} -> full's slot {} stride {}",
                      vs.numero, size_t(&elemento - vs.elementos.data()), elemento.instruccion, ranura, zancada, r0, z0);
          ranura = r0;
          zancada = z0;
        } else {
          // No previous full fetch found: the mini fetch keeps its own (meaningless) constant and stride.
          REXLOG_INFO("[vfmini] vs {} element {} (instr {}): mini fetch slot {} stride {} has NO previous full fetch",
                      vs.numero, size_t(&elemento - vs.elementos.data()), elemento.instruccion, ranura, zancada);
        }
      }
    }
    const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
    if (elemento.manual) {
      // VS 1632 reads BLENDINDICES0 using the same index as the surrounding instance fetches.
      // Test the unused word at byte 16 of that record instead of indexing the mesh stream.
      if (REXCVAR_GET(fc3_dk_grass_instance_blendindices) && vs.huella_contenedor == 0xAF5781B94E1A5994ull &&
          elemento.instruccion == 33 && elemento.uso == 2 && elemento.indice_uso == 0 && formato == 6 &&
          ranura == 95 && offset == 16 && zancada == 20 && !entrada.manuales.empty()) {
        const auto& instancia = entrada.manuales.back();  // COLOR1 fetch at instruction 32
        if (instancia.k == 8 && instancia.ranura == 94 && instancia.zancada == 32) {
          ranura = instancia.ranura;
          zancada = instancia.zancada;
        }
      }
      // Read by the shader at the index it computes (dsVfetch): the whole buffer as a storage buffer, described here.
      if (offset < 0) {
        return Rechazar(24, "offset de vertice negativo o demasiado grande");
      }
      uint32_t b = 0;
      while (b < entrada.ranuras_manuales.size() && entrada.ranuras_manuales[b] != ranura) {
        ++b;
      }
      if (b == entrada.ranuras_manuales.size()) {
        if (b >= 4) {
          return Rechazar(26, "mas de 4 bufers de vertices leidos por el propio shader");
        }
        entrada.ranuras_manuales.push_back(ranura);
      }
      EntradaVerticesDk::Manual m;
      m.k = uint32_t(&elemento - vs.elementos.data());
      m.ranura = ranura;
      m.formato = formato;
      m.offset = uint32_t(offset);
      m.zancada = zancada;
      m.remapeo = CodigoRemapeo(original, d1 & 0xFFF);
      m.con_signo = (d1 >> 12) & 0x1;
      m.entero = (d1 >> 13) & 0x1;
      // fc3_dk_grass_record_pair (EXPERIMENT, see the cvar): fixed record descriptors for VS 1632's r4 halves. The
      // patched words of instructions 31-36 are logged once ([grasspair]) to show where slot 95 comes from.
      if (REXCVAR_GET(fc3_dk_grass_record_pair) && vs.huella_contenedor == 0xAF5781B94E1A5994ull &&
          (elemento.instruccion == 33 || elemento.instruccion == 35)) {
        static uint32_t informes_par = 0;
        if (informes_par < 2 && elemento.instruccion == 33) {
          ++informes_par;
          for (uint32_t ins = 31; ins <= 36; ++ins) {
            const size_t j = size_t(ins) * 3;
            if (j + 2 >= parcheado.size()) break;
            const uint32_t w0 = parcheado[j], w1 = parcheado[j + 1], w2 = parcheado[j + 2];
            REXLOG_INFO("[grasspair] vs {} instr {} orig {:08X} {:08X} patched {:08X} {:08X} {:08X}: op {} dst r{} "
                        "src r{}.{} const {} fmt {} swz {:03X} signed {} int {} mini {} off {} stride {}",
                        vs.numero, ins, vs.microcodigo[j], vs.microcodigo[j + 1], w0, w1, w2, w0 & 0x1F,
                        (w0 >> 12) & 0x3F, (w0 >> 5) & 0x3F, (w0 >> 30) & 0x3, ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 0x3),
                        (w1 >> 16) & 0x3F, w1 & 0xFFF, (w1 >> 12) & 0x1, (w1 >> 13) & 0x1, (w1 >> 30) & 0x1,
                        (int32_t(w2 << 1) >> 9) * 4, (w2 & 0xFF) * 4);
          }
        }
        const uint32_t antes_ranura = m.ranura, antes_formato = m.formato, antes_offset = m.offset;
        const uint32_t antes_zancada = m.zancada, antes_remapeo = m.remapeo;
        m.ranura = 94;
        m.zancada = 32;
        if (elemento.instruccion == 33) {  // k9 -> r4.zw = 16_16 @20 of the record
          m.formato = 25;
          m.offset = 20;
          m.remapeo = kRemapeoIdentidad;
          m.con_signo = true;
          m.entero = false;
        } else {  // k11 -> r4.xy = 8_8_8_8 @16 of the record
          m.formato = 6;
          m.offset = 16;
          m.remapeo = 0xFCA;
          m.con_signo = false;
          m.entero = false;
        }
        static uint32_t informes_cambio = 0;
        if (informes_cambio < 8) {
          ++informes_cambio;
          REXLOG_INFO("[grasspair] vs {} k {} instr {}: slot {} fmt {} off {} stride {} remap {:03X} -> slot {} fmt {} off {} "
                      "stride {} remap {:03X} signed {}",
                      vs.numero, m.k, elemento.instruccion, antes_ranura, antes_formato, antes_offset, antes_zancada,
                      antes_remapeo, m.ranura, m.formato, m.offset, m.zancada, m.remapeo, m.con_signo);
        }
        uint32_t c = 0;
        while (c < entrada.ranuras_manuales.size() && entrada.ranuras_manuales[c] != m.ranura) {
          ++c;
        }
        if (c == entrada.ranuras_manuales.size()) {
          if (c >= 4) {
            return Rechazar(26, "mas de 4 bufers de vertices leidos por el propio shader");
          }
          entrada.ranuras_manuales.push_back(m.ranura);
        }
      }
      m.componentes16 = (m.formato == 25 || m.formato == 31) ? 2 : (m.formato == 26 || m.formato == 32) ? 4 : 0;
      entrada.manuales.push_back(m);
      continue;
    }
    // Controlled terrain experiment: the material and depth-prepass vertex
    // shaders fetch PSIZE0, but the original library replaces it with a
    // constant. Location 2 is unused by these two shaders and is used by the
    // paired experimental SPIR-V modules.
    const bool psize_terreno = REXCVAR_GET(fc3_terrain_psize_experiment) &&
                               (vs.huella_contenedor == 0x8A32309FF85D723Aull ||
                                vs.huella_contenedor == 0xE7963F38129B59CBull) &&
                               elemento.uso == 4 && elemento.indice_uso == 0;
    int32_t ubicacion = psize_terreno ? 2 : UbicacionDeUso(elemento.uso, elemento.indice_uso);
    if (REXCVAR_GET(fc3_extra_vertex_semantics) && (elemento.uso == 4 || elemento.uso == 11) &&
        elemento.indice_uso < 16) {
      ubicacion = extra_ubicaciones[elemento.uso == 4 ? 0 : 1][elemento.indice_uso];
    }
    if (ubicacion < 0 || ubicacion >= 16 || ((ubicaciones_usadas >> ubicacion) & 0x1)) {
      Avisar(22, "elemento de vertices sin ubicacion libre en el shader: se omite");
      continue;
    }
    DkVtxAttribSize tamano;
    DkVtxAttribType tipo;
    bool r11g11b10 = false;
    if (!AtributoDe(formato, EntradaEntera(elemento.uso), (d1 >> 12) & 0x1, (d1 >> 13) & 0x1, tamano, tipo,
                    r11g11b10)) {
      return Rechazar(300 + formato, "formato de vertice todavia no soportado");
    }
    if (r11g11b10) {
      entrada.especializacion |= 0x1;
    }
    // fc3-switch 2026-11-08 DIAGNOSTIC: VS 1051's (and the other per-object vegetation VS's) vertex attributes as
    // bound: usage/index, location, fetch slot, Xenos format, offset, stride, remap, signed/integer, exp adjust.
    if (vs.numero == 1051 || vs.numero == 1495 || vs.numero == 1632 || vs.numero == 1213 || vs.numero == 5 ||
        vs.numero == 1668) {
      REXLOG_INFO("[vsattr] vs {} uso {}:{} loc {} slot {} fmt {} off {} stride {} remap {:03X} signed {} int {} exp {} "
                  "endian-in-d1 {}",
                  vs.numero, elemento.uso, elemento.indice_uso, ubicacion, ranura, formato, offset, zancada,
                  CodigoRemapeo(original, d1 & 0xFFF), (d1 >> 12) & 0x1, (d1 >> 13) & 0x1,
                  int32_t(d1 << 2) >> 26, (d0 >> 25) & 0x3);
    }
    if (offset < 0 || offset >= (1 << 14)) {
      return Rechazar(24, "offset de vertice negativo o demasiado grande");
    }
    uint32_t enlace = 0;
    while (enlace < entrada.enlaces.size() && entrada.enlaces[enlace].ranura != ranura) {
      ++enlace;
    }
    const bool indice_cero = texcoords_cero && elemento.uso == 5 && elemento.indice_uso < 16 &&
                             ((texcoords_cero >> elemento.indice_uso) & 0x1);
    if (enlace == entrada.enlaces.size()) {
      if (enlace >= 16) {
        return Rechazar(9, "demasiados streams de vertices");
      }
      entrada.enlaces.push_back({ranura, zancada});
      entrada.enlaces.back().indice_cero = indice_cero;
    } else {
      if (!entrada.enlaces[enlace].zancada) {
        entrada.enlaces[enlace].zancada = zancada;
      }
      if (entrada.enlaces[enlace].indice_cero != indice_cero) {
        entrada.enlaces[enlace].mixto = true;
      }
    }
    DkVtxAttribState& a = entrada.atributos[uint32_t(ubicacion)];
    a = Ceros<DkVtxAttribState>();
    a.bufferId = enlace;
    a.isFixed = 0;
    a.offset = uint32_t(offset);
    a.size = tamano;
    a.type = tipo;
    entrada.n_atributos = std::max(entrada.n_atributos, uint32_t(ubicacion) + 1);
    entrada.remapeos[uint32_t(ubicacion)] = CodigoRemapeo(original, d1 & 0xFFF);
    if (EntradaEntera(elemento.uso) && !((d1 >> 13) & 0x1)) {
      const bool con_signo_bi = (d1 >> 12) & 0x1;
      const uint32_t bits = formato == 6 ? 8 : formato == 7 ? 10 : (formato == 25 || formato == 26) ? 16 : 0;
      if (bits) {
        entrada.escala_indices = 1.0f / float((1u << (con_signo_bi ? bits - 1 : bits)) - 1u);
      }
    }
    if (formato == 25 || formato == 31) {
      entrada.mitades16 |= uint32_t(1) << ubicacion;
    } else if (formato == 26 || formato == 32) {
      entrada.mitades16 |= uint32_t(1) << (ubicacion + 16);
    }
    entrada.lista.push_back({uint32_t(ubicacion), enlace, uint32_t(offset)});
    ubicaciones_usadas |= uint32_t(1) << ubicacion;
  }
  for (size_t b = 0; b < entrada.enlaces.size(); ++b) {
    EntradaVerticesDk::Enlace& e = entrada.enlaces[b];
    if (e.mixto) {
      e.indice_cero = false;
    }
    if (texcoords_cero) {
      // fc3-switch DIAGNOSTIC: one line per shader/stream the table covers.
      REXLOG_INFO("[deko3d] indice cero: vs={} contenedor={:016X} slot={} stride={} indice_cero={} mixto={}", vs.numero,
                  vs.huella_contenedor, e.ranura, e.zancada, e.indice_cero, e.mixto);
    }
    if (!e.zancada && !REXCVAR_GET(fc3_dk_stride0)) {
      return Rechazar(25, "stream de vertices sin zancada");
    }
    entrada.estado_bufers[b] = {e.indice_cero ? 0u : e.zancada, 0};
  }
  return true;
}

// ---- Framing (the Vulkan backend's CalcularEncuadre; deko3d receives the same numbers: its clip-space y points
// up like Direct3D's, and the library is compiled without -fvk-invert-y). viewport = {x, y, width, height, near,
// far}; the height may come out negative (a flipped target): the recording turns that into a viewport swizzle.

uint32_t RenderDk::CalcularEncuadre(const uint32_t* r, uint32_t vte, bool sin_recorte, uint32_t modo_sc,
                                    float viewport[6], float ndc[4], DkScissor& tijera) {
  float escala_x = (vte & 0x1) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
  float centro_x = (vte & 0x2) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
  float escala_y = (vte & 0x4) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
  float centro_y = (vte & 0x8) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
  const float escala_z = (vte & 0x10) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
  const float centro_z = (vte & 0x20) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
  const uint32_t ventana = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  const int32_t ventana_x = int32_t((ventana & 0x7FFF) << 17) >> 17;
  const int32_t ventana_y = int32_t(((ventana >> 16) & 0x7FFF) << 17) >> 17;
  if ((modo_sc >> 16) & 0x1) {
    centro_x += float(ventana_x);
    centro_y += float(ventana_y);
  }
  if (pase_escala_ != 1.0f) {
    escala_x *= pase_escala_;
    centro_x *= pase_escala_;
    escala_y *= pase_escala_;
    centro_y *= pase_escala_;
  }
  viewport[0] = centro_x - std::abs(escala_x);
  viewport[2] = 2.0f * std::abs(escala_x);
  // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
  viewport[1] = centro_y + escala_y;
  viewport[3] = -2.0f * escala_y;
  if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
    Avisar(14, "espacio de recorte OpenGL (dx_clip_space_def = 0): Z sin ajustar");
  }
  viewport[4] = std::clamp(centro_z, 0.0f, 1.0f);
  viewport[5] = std::clamp(centro_z + escala_z, 0.0f, 1.0f);
  ndc[0] = 1.0f;
  ndc[1] = 1.0f;
  ndc[2] = 0.0f;
  ndc[3] = 0.0f;
  const uint32_t pase_ancho = g_.pase_ancho, pase_alto = g_.pase_alto;
  if (sin_recorte) {
    const float ancho = float(pase_ancho);
    const float alto = float(pase_alto);
    ndc[0] = escala_x * 2.0f / ancho;
    ndc[2] = (centro_x - ancho * 0.5f) * 2.0f / ancho;
    ndc[1] = -escala_y * 2.0f / alto;
    ndc[3] = -(centro_y - alto * 0.5f) * 2.0f / alto;
    viewport[0] = 0.0f;
    viewport[1] = 0.0f;
    viewport[2] = ancho;
    viewport[3] = alto;
  }
  tijera = DkScissor{};
  if (viewport[2] < 1.0f || std::abs(viewport[3]) < 1.0f) {
    return 1;
  }
  const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
  int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
  if (!((tl >> 31) & 0x1)) {
    x0 += ventana_x;
    y0 += ventana_y;
    x1 += ventana_x;
    y1 += ventana_y;
  }
  if (pase_escala_ != 1.0f) {
    x0 = int32_t(std::floor(float(x0) * pase_escala_));
    y0 = int32_t(std::floor(float(y0) * pase_escala_));
    x1 = int32_t(std::ceil(float(x1) * pase_escala_));
    y1 = int32_t(std::ceil(float(y1) * pase_escala_));
  }
  x0 = std::clamp(x0, 0, int32_t(pase_ancho));
  x1 = std::clamp(x1, 0, int32_t(pase_ancho));
  y0 = std::clamp(y0, 0, int32_t(pase_alto));
  y1 = std::clamp(y1, 0, int32_t(pase_alto));
  if (x1 <= x0 || y1 <= y0) {
    return 2;
  }
  tijera = DkScissor{uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
  return 0;
}

// ---- Render targets of a draw. Nothing to close or open: binding others is all a pass change costs.

bool RenderDk::EmpezarPase(const uint64_t claves[5], uint32_t pitch) {
  EmitirCieloPendiente();  // the deferred sky belongs to the pass that ends here
  std::array<ImagenDk*, 5> imagenes{};
  for (uint32_t i = 0; i < 4; ++i) {
    if (claves[i]) {
      imagenes[i] = DestinoColor(uint32_t(claves[i] >> 24) & 0xFFF, uint32_t(claves[i] >> 16) & 0xF, pitch);
      if (!imagenes[i]) {
        return Rechazar(40, "no hay destino de color");
      }
    }
  }
  if (claves[4]) {
    imagenes[4] = DestinoProfundidad(uint32_t(claves[4] >> 24) & 0xFFF, uint32_t(claves[4] >> 16) & 0x1, pitch);
    if (!imagenes[4]) {
      return Rechazar(41, "no hay destino de profundidad");
    }
  }
  // fc3-switch (PROTOTYPE): EDRAM aliasing of an 8888 color target and the depth target at its base (alias_color_z_
  // in render.h). Color passes mark their 8888 bases; a pass that binds depth at a marked base without that color
  // gets the color's bytes as depth and stencil first.
  for (uint32_t i = 0; i < 4; ++i) {
    if (claves[i] && ((claves[i] >> 16) & 0xF) == 0) {
      alias_color_z_.insert((((claves[i] >> 24) & 0xFFF) << 16) | pitch);
      colores_8888_.insert((((claves[i] >> 24) & 0xFFF) << 16) | pitch);
    }
  }
  if (imagenes[4]) {
    const uint64_t base_z = (claves[4] >> 24) & 0xFFF;
    const auto alias = alias_color_z_.find((base_z << 16) | pitch);
    bool color_atado = false;
    for (uint32_t i = 0; i < 4; ++i) {
      color_atado |= claves[i] && ((claves[i] >> 24) & 0xFFF) == base_z;
    }
    if (alias != alias_color_z_.end() && !color_atado) {
      alias_color_z_.erase(alias);
      if (ImagenDk* color = DestinoColor(uint32_t(base_z), 0, pitch)) {
        HacerBorradoPendiente(*imagenes[4]);  // a clear asked for before the color was drawn must not wipe it after
        const uint32_t ancho_c = std::min(color->ancho, imagenes[4]->ancho);
        uint32_t alto_c = std::min(color->alto, imagenes[4]->alto);
        // fc3_dk_copiar_alto_usado: only the rows either target is used at (1280x704 of a 1280x1280 target).
        if (REXCVAR_GET(fc3_dk_copiar_alto_usado) && color->alto_usado && imagenes[4]->alto_usado) {
          alto_c = std::min(alto_c, std::max(color->alto_usado, imagenes[4]->alto_usado));
        }
        const bool checkpoint = volcando_ && base_z == 0x540 && pitch == 1280 &&
                                REXCVAR_GET(fc3_trace_lighting_checkpoint);
        if (checkpoint) VolcarResuelta(*color, 0xFC054000u, 6);
        const int32_t modos[4] = {int32_t((claves[4] >> 16) & 0x1), 0, 0, 0};
        tipo_copia_ = "alias color->z";
        if (Copiar3D(*color, 0, 0, ancho_c, alto_c, *imagenes[4], 0, 0, modos, false, 1.0f, false, true)) {
          if (checkpoint) {
            VolcarResuelta(*imagenes[4], 0xFC054001u, 23);
            REXLOG_INFO("[lighting-checkpoint] import frame={} z={} cleared={} used={} dirty={} pending={}",
                        fotograma_, imagenes[4]->serie, imagenes[4]->alto_borrado, imagenes[4]->alto_usado,
                        imagenes[4]->banda_sucia, imagenes[4]->borrado_pendiente);
          }
          if (alias_color_z_copias_++ < 8) {
            REXLOG_INFO("[deko3d] color 8888 en {:03X} (pitch {}) copiado a profundidad y estencil ({}x{}, formato {})",
                        base_z, pitch, ancho_c, alto_c, (claves[4] >> 16) & 0x1);
          }
        }
      }
    }
  }
  // A target whose content a resolve without copy gave away gets it back before it is drawn on (the shadow map's car
  // pass, while the shadow minimum applies, gets a pending clear instead).
  for (ImagenDk* imagen : imagenes) {
    if (imagen) {
      AntesDeCambiarDestino(imagen);  // the front buffer still painted from it gets its copy first
      RestaurarContenido(*imagen, false);
    }
  }
  AntesDe3D();
  // Creating a target records its first clear and forgets the emitted state: the binding comes after.
  uint32_t ancho = UINT32_MAX, alto = UINT32_MAX;
  DkImageView vistas[4];
  const DkImageView* colores[4] = {};
  uint32_t n = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (imagenes[i]) {
      dkImageViewDefaults(&vistas[i], &imagenes[i]->imagen);
      colores[i] = &vistas[i];
      n = i + 1;  // preserve the guest MRT slot used by PS output locations
      ancho = std::min(ancho, imagenes[i]->ancho);
      alto = std::min(alto, imagenes[i]->alto);
    }
  }
  // deko3d requires non-null views below n. Inactive slots alias a bound view
  // as a placeholder, with their color writes and blending disabled below.
  // PS output locations must never shift when an earlier MRT slot is inactive.
  const DkImageView* relleno = nullptr;
  for (uint32_t i = 0; i < n && !relleno; ++i) relleno = colores[i];
  for (uint32_t i = 0; i < n; ++i) if (!colores[i]) colores[i] = relleno;
  DkImageView vista_z;
  if (imagenes[4]) {
    dkImageViewDefaults(&vista_z, &imagenes[4]->imagen);
    ancho = std::min(ancho, imagenes[4]->ancho);
    alto = std::min(alto, imagenes[4]->alto);
  }
  const uint32_t categoria = CategoriaDeDestino(pitch, claves);  // GPU time by pass type (the C2 report)
  categoria_pase_ = categoria;
  const bool borrar_z = imagenes[4] && imagenes[4]->borrado_pendiente;
  MarcarGpu(borrar_z ? kGpuBorrados : categoria);
  DkCmdBuf cmd = nucleo_.comandos();
  dkCmdBufBindRenderTargets(cmd, colores, n, imagenes[4] ? &vista_z : nullptr);
  // The clears the game asked for when it resolved, done now that the targets are bound (nfsmw_dk_borrado_z_aplazado,
  // nfsmw_dk_borrado_color_aplazado): the depth's ZCULL data starts the pass valid. Only the useful area.
  bool borrado = false;
  for (uint32_t i = 0; i < 4; ++i) {
    if (imagenes[i]) {
      if (imagenes[i]->borrado_pendiente) {
        if (!borrado && !borrar_z) {
          MarcarGpu(kGpuBorrados);
        }
        imagenes[i]->borrado_pendiente = false;
        const uint32_t h = imagenes[i]->alto_borrado == 0 ? imagenes[i]->alto : AltoDeBorrado(*imagenes[i]);
        BorrarFilas(*imagenes[i], i, 0, h);
        borrado = true;
      }
    }
  }
  if (borrar_z) {
    ImagenDk& z = *imagenes[4];
    z.borrado_pendiente = false;
    const uint32_t h = z.alto_borrado == 0 ? z.alto : AltoDeBorrado(z);
    BorrarFilas(z, 0, 0, h);
    borrado = true;
  }
  if (borrado) {
    MarcarGpu(categoria);
    g_.valido = false;  // the scissor is the clear's: this draw emits all of its state again
  }
  imagenes_pase_ = imagenes;
  // A new pass for the deferred sky's "first draw of the pass" only when the targets change, not when the same ones
  // are bound again after a copy.
  if (borrar_z || std::memcmp(claves_pase_previo_, claves, sizeof(claves_pase_previo_)) != 0) {
    std::copy(claves, claves + 5, claves_pase_previo_);
    dibujos_en_pase_ = 0;
  }
  // nfsmw_nativo_sombra_minimo: if this is the car pass of the shadow map, its draws are checked.
  const bool solo_profundidad = pitch >= kLadoSombrasDibujos && claves[4] && !claves[0] && !claves[1] && !claves[2] &&
                                !claves[3];
  pase_coches_sombra_ = imagenes[4] && PaseDeCochesSombra(imagenes[4], solo_profundidad);
  std::copy(claves, claves + 5, g_.pase);
  g_.pase_valido = true;
  g_.pase_ancho = ancho;
  g_.pase_alto = alto;
  ++serie_imagenes_;  // a new pass: the framing cache keys on it
  g_.pase_serie = serie_imagenes_;
  pase_escala_ = 1.0f;
  return true;
}

// ---- The deferred sky

void RenderDk::EmitirCieloPendiente() {
  if (!cielo_pendiente_) {
    return;
  }
  cielo_pendiente_ = false;
  ++cielo_emitidos_;
  const CieloDk& c = cielo_;
  const Grabado& e = c.estado;
  DkCmdBuf cmd = nucleo_.comandos();
  // Everything it would have emitted in its own place, from what it left in g_; then nothing tracked holds.
  const DkShader* shaders[2] = {e.vs, e.ps};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindRasterizerState(cmd, &e.raster);
  dkCmdBufBindColorState(cmd, &e.color);
  dkCmdBufBindColorWriteState(cmd, &e.mascaras);
  for (uint32_t i = 0; i < 4; ++i) {
    if ((e.color.blendEnableMask >> i) & 0x1) {
      dkCmdBufBindBlendState(cmd, i, &e.mezclas[i]);
    }
  }
  dkCmdBufBindDepthStencilState(cmd, &e.profundidad);
  if (e.profundidad.stencilTestEnable) {
    dkCmdBufSetStencil(cmd, DkFace_Front, uint8_t(e.stencil[0] >> 16), uint8_t(e.stencil[0]), uint8_t(e.stencil[0] >> 8));
    dkCmdBufSetStencil(cmd, DkFace_Back, uint8_t(e.stencil[1] >> 16), uint8_t(e.stencil[1]), uint8_t(e.stencil[1] >> 8));
  }
  dkCmdBufSetBlendConst(cmd, e.mezcla_constante[0], e.mezcla_constante[1], e.mezcla_constante[2],
                        e.mezcla_constante[3]);
  if (e.raster.depthBiasEnableMask) {
    dkCmdBufSetDepthBias(cmd, e.sesgo[0], 0.0f, e.sesgo[1]);
  }
  dkCmdBufSetPrimitiveRestart(cmd, e.reinicio, 0xFFFFFFFFu);
  const DkViewport v = {e.viewport[0], e.viewport[1], e.viewport[2], e.viewport[3], e.viewport[4], e.viewport[5]};
  dkCmdBufSetViewports(cmd, 0, &v, 1);
  const DkViewportSwizzle sw = {DkSwizzle_PositiveX, e.volteado ? DkSwizzle_NegativeY : DkSwizzle_PositiveY,
                                DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &sw, 1);
  dkCmdBufSetScissors(cmd, 0, &e.tijera, 1);
  dkCmdBufBindVtxAttribState(cmd, e.entrada->atributos.data(), e.entrada->n_atributos);
  dkCmdBufBindVtxBufferState(cmd, e.entrada->estado_bufers.data(), uint32_t(e.entrada->enlaces.size()));
  if (e.n_texturas) {
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, e.texturas.data(), e.n_texturas);
  }
  dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, e.ubo[0], kUboBytesVs);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, e.ubo[1], kUboBytesPs);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, e.ubo[2], kUboBytesCompartidas);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, e.ubo[2], kUboBytesCompartidas);
  for (uint32_t b = 0; b < c.n_bufers; ++b) {
    dkCmdBufBindVtxBuffers(cmd, b, &c.bufers[b], 1);  // one per call (deko3d 0.5.0, see Dibujar)
  }
  if (c.con_indices) {
    dkCmdBufBindIdxBuffer(cmd, c.indices_de_16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, c.indices);
    dkCmdBufDrawIndexed(cmd, c.primitiva, c.n_indices, 1, 0, -int32_t(c.vmin), 0);
  } else {
    dkCmdBufDraw(cmd, c.primitiva, c.cuenta, 1, 0, 0);
  }
  ++dibujos_en_pase_;
  // The next draw emits all of its own state again; the pass stays bound. What it compares without g_.valido (the
  // shaders and the uniform buffers) is what the sky left bound.
  g_.valido = false;
  g_.vs = e.vs;
  g_.ps = e.ps;
  std::copy(std::begin(e.ubo), std::end(e.ubo), std::begin(g_.ubo));
}

// Once per frame (Presentar): the guard counts the frames in which the dome appears.
void RenderDk::CerrarFotogramaDeLaGuardiaDelCielo() {
  const uint32_t en_este = cielo_guardia_en_fotograma_;
  cielo_guardia_en_fotograma_ = 0;
  if (cielo_guardia_ != kCieloMirando || en_este == 0) {
    return;  // a frame without the dome (menus, loading) says nothing
  }
  cielo_guardia_max_ = std::max(cielo_guardia_max_, en_este);
  if (en_este == 1) {
    ++cielo_guardia_con_uno_;
  }
  if (++cielo_guardia_fotogramas_ < kCieloFotogramasPrueba) {
    return;
  }
  if (cielo_guardia_max_ == 1 && cielo_guardia_con_uno_ >= kCieloFotogramasPrueba) {
    cielo_guardia_ = kCieloAplazando;
    REXLOG_INFO("[deko3d] cielo: guardia SUPERADA ({} fotogramas con exactamente un domo, nunca dos); se aplaza el "
                "cielo de la escena a partir de ahora",
                cielo_guardia_con_uno_);
  } else {
    cielo_guardia_ = kCieloDescartado;
    REXLOG_WARN("[deko3d] cielo: guardia NO superada (maximo {} domos en un fotograma, {} de {} con uno); no se "
                "aplaza nada",
                cielo_guardia_max_, cielo_guardia_con_uno_, cielo_guardia_fotogramas_);
  }
}

// ---- Shaders

RenderDk::InfoShader& RenderDk::InfoDe(const EntradaShader& entrada) {
  InfoShader& info = info_shaders_[&entrada];
  if (!info.huella) {
    const auto& espirv = entrada.spirv;
    info.huella = deko::HuellaSpirv(espirv);
    info.mascara = deko::MascaraEspecializacion(espirv);
  }
  if (!entrada.vertices && !info.interpoladores_listos) {
    // What the pixel shader reads: the vertex shader variant (the PC tool does the same, nfsmw_dksh.cpp).
    info.interpoladores = deko::SpirvAGlsl(entrada.spirv, 0).interpoladores;
    info.interpoladores_listos = true;
  }
  return info;
}

const DkShader* RenderDk::ShaderDe(const EntradaShader& entrada, uint32_t especializacion, uint32_t salidas,
                                   uint32_t variante_ps) {
  InfoShader& info = InfoDe(entrada);
  // The SPIR-V: the library's, or the bright pass replaced by a glow variant (same inputs and constants, so the
  // library shader's mask applies).
  std::span<const uint32_t> espirv(entrada.spirv);
  if (variante_ps) {
    const std::span<const uint32_t> v = hooks_.SpirvDeVariante(variante_ps);
    if (!v.empty()) {
      espirv = v;
    }
  }
  const uint32_t espec = especializacion & info.mascara;
  const uint32_t variante = entrada.vertices ? salidas : variante_ps;
  const ClaveCargado clave{&entrada, espec, variante};
  if (const auto it = cargados_.find(clave); it != cargados_.end()) {
    return it->second.get();
  }
  // fc3-switch 2026-11-11 DIAGNOSTIC (fc3_dk_uam_sin_postra_ps, test T2): listed PS get a marker line in front of
  // their GLSL (the compile worker strips it and skips uam's PostRADualIssue) and a separate cache key.
  bool sin_postra = false;
  if (!entrada.vertices) {
    static std::vector<uint32_t> lista_ps;
    static bool leido = false;
    if (!leido) {
      leido = true;
      const std::string lista = REXCVAR_GET(fc3_dk_uam_sin_postra_ps);
      size_t i = 0;
      while (i < lista.size()) {
        const size_t j = lista.find(',', i);
        const std::string t = lista.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!t.empty()) lista_ps.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 10)));
        if (j == std::string::npos) break;
        i = j + 1;
      }
      if (!lista_ps.empty()) REXLOG_WARN("[deko3d] fc3_dk_uam_sin_postra_ps: {} PS (DIAGNOSTIC)", lista_ps.size());
    }
    sin_postra = std::find(lista_ps.begin(), lista_ps.end(), entrada.numero) != lista_ps.end();
  }
  const deko::CacheDksh::Clave clave_cache{info.huella ^ (sin_postra ? 0x5053545241444931ull : 0), espec, variante};
  auto marcar_postra = [sin_postra](std::string& glsl) {
    if (sin_postra) glsl.insert(0, "//fc3:uam-sin-postra\n");
  };
  char nombre[64];
  std::snprintf(nombre, sizeof(nombre), "%s n%u %08X %08X", entrada.vertices ? "VS" : "PS", entrada.numero, espec,
                variante);
  auto cargar = [&](const std::vector<uint8_t>& dksh) -> const DkShader* {
    auto shader = std::make_unique<DkShader>();
    if (!nucleo_.CargarShader(*shader, dksh, nombre)) {
      shader.reset();
    }
    const DkShader* puntero = shader.get();
    if (puntero) {
      // For the GPU profile: uam's DkshHeader (code_sz at 12, programs_off at 16) and the program header's num_gprs
      // (at 8) and per_warp_scratch_sz (at 20).
      DatosShaderDk datos{info.huella, entrada.numero, espec, variante, entrada.vertices};
      uint32_t cabecera[6] = {};
      if (dksh.size() >= sizeof(cabecera)) {
        std::memcpy(cabecera, dksh.data(), sizeof(cabecera));
        datos.bytes_codigo = cabecera[3];
        uint32_t programa[6] = {};
        if (uint64_t(cabecera[4]) + sizeof(programa) <= dksh.size()) {
          std::memcpy(programa, dksh.data() + cabecera[4], sizeof(programa));
          datos.registros = programa[2];
          datos.scratch = programa[5];
        }
      }
      datos_shaders_[puntero] = datos;
    }
    cargados_.emplace(clave, std::move(shader));
    return puntero;
  };
  if (const std::vector<uint8_t>* dksh = cache_.Buscar(clave_cache)) {
    ++aciertos_cache_;
    return cargar(*dksh);
  }
  static const bool en_fondo = REXCVAR_GET(nfsmw_dk_compilar_en_fondo);
  if (entrada.vertices && variante != kTodasLasSalidas && en_fondo &&
      cache_.Buscar({info.huella, espec, kTodasLasSalidas})) {
    // The variant with every interpolator draws the same (the pixel shader ignores the rest) while the exact one
    // compiles on the worker; it is not remembered here, so the next draw looks in the cache again.
    const uint64_t h = XXH3_64bits(&clave_cache, sizeof(clave_cache));
    if (en_fondo_.emplace(h, true).second) {
      // Each one logged (once per session): the variants the PC tool's cache lacks, to add them there.
      REXLOG_INFO("[deko3d] {} no esta en la cache: se compila en segundo plano (huella {:016X})", nombre, info.huella);
      const deko::GlslDeko g = deko::SpirvAGlsl(entrada.spirv, espec, variante);
      if (!g.glsl.empty()) {
        std::weak_ptr<Bandeja> bandeja = bandeja_;
        compilador_.Encolar(true, g.glsl, [bandeja, clave_cache](deko::Compilador::Resultado r) {
          if (const auto b = bandeja.lock()) {
            std::lock_guard<std::mutex> cerrojo(b->candado);
            if (r.ok) {
              b->hechas.emplace_back(clave_cache, std::move(r.dksh));
            } else {
              ++b->fallidas;
            }
          }
        });
      }
    }
    return ShaderDe(entrada, especializacion, kTodasLasSalidas);
  }
  if (!entrada.vertices && variante_ps && en_fondo) {
    // A pixel shader variant draws as the library's shader while it compiles on the worker: the same image (early
    // depth only changes speed) or, for the glow, the game's own bright pass for a few frames.
    const uint64_t h = XXH3_64bits(&clave_cache, sizeof(clave_cache));
    if (en_fondo_.emplace(h, true).second) {
      REXLOG_INFO("[deko3d] {} no esta en la cache: se compila en segundo plano (huella {:016X})", nombre, info.huella);
      deko::GlslDeko g = deko::SpirvAGlsl(espirv, espec, kTodasLasSalidas);
      if (!g.glsl.empty()) {
        if (variante_ps & kVarianteZTemprana) {
          DeclararZTemprana(g.glsl);
        }
        marcar_postra(g.glsl);
        std::weak_ptr<Bandeja> bandeja = bandeja_;
        compilador_.Encolar(false, g.glsl, [bandeja, clave_cache](deko::Compilador::Resultado r) {
          if (const auto b = bandeja.lock()) {
            std::lock_guard<std::mutex> cerrojo(b->candado);
            if (r.ok) {
              b->hechas.emplace_back(clave_cache, std::move(r.dksh));
            } else {
              ++b->fallidas;
            }
          }
        });
      }
    }
    return ShaderDe(entrada, especializacion, salidas, 0);
  }
  // fc3-switch (PROTOTYPE, 2026-11-02): never on the spot. Run 24 hung for good when the world loaded: uam sat in
  // nv50_ir::GCRA::buildRIG on one shader (the Dead Space 2 case above, minutes) and the ring thread waited for it.
  // Here the shader compiles on the worker and its draws are skipped ("shader sin DKSH") until RecogerCompilaciones
  // puts it in the cache; a shader that never finishes only loses its own draws (and delays the queue behind it).
  static const bool nunca_en_el_acto = REXCVAR_GET(nfsmw_dk_nunca_en_el_acto);
  if (nunca_en_el_acto) {
    const uint64_t h = XXH3_64bits(&clave_cache, sizeof(clave_cache));
    if (en_fondo_.emplace(h, true).second) {
      // Logged before it starts: the last of these lines names the shader a stuck compile is on.
      REXLOG_INFO("[deko3d] {} se compila en segundo plano sin esperar (huella {:016X}, {} en cola)", nombre,
                  info.huella, compilador_.Pendientes());
      deko::GlslDeko g = deko::SpirvAGlsl(espirv, espec, entrada.vertices ? variante : kTodasLasSalidas);
      if (g.glsl.empty()) {
        REXLOG_ERROR("[deko3d] {}: no se pudo pasar a GLSL: {}", nombre, g.error);
        cargados_.emplace(clave, nullptr);
        return nullptr;
      }
      if (!entrada.vertices && (variante_ps & kVarianteZTemprana)) {
        DeclararZTemprana(g.glsl);
      }
      marcar_postra(g.glsl);  // DIAGNOSTIC T2
      if (sin_postra) REXLOG_WARN("[deko3d] {}: compiled without uam PostRADualIssue (DIAGNOSTIC T2)", nombre);
      std::weak_ptr<Bandeja> bandeja = bandeja_;
      compilador_.Encolar(entrada.vertices, std::move(g.glsl), [bandeja, clave_cache](deko::Compilador::Resultado r) {
        if (const auto b = bandeja.lock()) {
          std::lock_guard<std::mutex> cerrojo(b->candado);
          if (r.ok) {
            b->hechas.emplace_back(clave_cache, std::move(r.dksh));
          } else {
            ++b->fallidas;
          }
        }
      });
    }
    return nullptr;
  }
  // On the spot (it blocks the ring thread for one compile: ~40-100 ms on the console).
  const uint64_t inicio = deko::AhoraNs();
  deko::GlslDeko g = deko::SpirvAGlsl(espirv, espec, entrada.vertices ? variante : kTodasLasSalidas);
  if (g.glsl.empty()) {
    REXLOG_ERROR("[deko3d] {}: no se pudo pasar a GLSL: {}", nombre, g.error);
    cargados_.emplace(clave, nullptr);
    return nullptr;
  }
  if (!entrada.vertices && (variante_ps & kVarianteZTemprana)) {
    DeclararZTemprana(g.glsl);
  }
  marcar_postra(g.glsl);  // DIAGNOSTIC T2
  const deko::Compilador::Resultado r = compilador_.Compilar(entrada.vertices, g.glsl);
  if (!r.ok) {
    REXLOG_ERROR("[deko3d] {}: uam no lo compila: {}", nombre, r.log);
    cargados_.emplace(clave, nullptr);
    return nullptr;
  }
  ++compilados_en_el_acto_;
  if (compilados_en_el_acto_ <= 64) {
    REXLOG_INFO("[deko3d] {} compilado en el acto en {:.1f} ms ({} bytes de DKSH)", nombre,
                double(deko::AhoraNs() - inicio) / 1e6, r.dksh.size());
  }
  cache_.Anadir(clave_cache, r.dksh);
  cache_consola_.Anadir(clave_cache, r.dksh);
  cache_sucia_ = true;
  return cargar(r.dksh);
}

void RenderDk::RecogerCompilaciones() {
  std::vector<std::pair<deko::CacheDksh::Clave, std::vector<uint8_t>>> hechas;
  uint32_t fallidas = 0;
  {
    std::lock_guard<std::mutex> cerrojo(bandeja_->candado);
    hechas.swap(bandeja_->hechas);
    fallidas = bandeja_->fallidas;
    bandeja_->fallidas = 0;
  }
  for (auto& [clave, dksh] : hechas) {
    cache_consola_.Anadir(clave, dksh);
    cache_.Anadir(clave, std::move(dksh));
    ++compilados_en_fondo_;
    cache_sucia_ = true;
  }
  if (fallidas) {
    REXLOG_WARN("[deko3d] {} compilaciones en segundo plano fallidas (se sigue con la variante completa)", fallidas);
  }
}

// ---- The draw

bool RenderDk::Dibujar(const PeticionDibujo& p) {
  const uint32_t* r = p.registros;
  if (!r || !p.vs || p.vs->spirv.empty()) {
    return false;
  }
  // fc3-switch 2026-11-08 DIAGNOSTIC (triangle hunt): fc3_dk_saltar_vs = "n,n,..." skips every draw whose VS is one of
  // those library numbers (bisection of the draw that paints the big flat polygons). Empty = off.
  {
    static std::vector<uint32_t> saltar;
    static bool leido = false;
    if (!leido) {
      leido = true;
      const std::string lista = REXCVAR_GET(fc3_dk_saltar_vs);
      size_t i = 0;
      while (i < lista.size()) {
        const size_t j = lista.find(',', i);
        const std::string t = lista.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!t.empty()) saltar.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 10)));
        if (j == std::string::npos) break;
        i = j + 1;
      }
      if (!saltar.empty()) REXLOG_WARN("[deko3d] fc3_dk_saltar_vs: {} VS skipped (DIAGNOSTIC)", saltar.size());
    }
    if (!saltar.empty() && p.vs && std::find(saltar.begin(), saltar.end(), p.vs->numero) != saltar.end()) {
      return true;
    }
  }
  // fc3-switch 2026-11-11 DIAGNOSTIC (AO specks): the same by PS number (VS 2131 draws both PS 1937 and PS 760).
  {
    static std::vector<uint32_t> saltar;
    static bool leido = false;
    if (!leido) {
      leido = true;
      const std::string lista = REXCVAR_GET(fc3_dk_saltar_ps);
      size_t i = 0;
      while (i < lista.size()) {
        const size_t j = lista.find(',', i);
        const std::string t = lista.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!t.empty()) saltar.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 10)));
        if (j == std::string::npos) break;
        i = j + 1;
      }
      if (!saltar.empty()) REXLOG_WARN("[deko3d] fc3_dk_saltar_ps: {} PS skipped (DIAGNOSTIC)", saltar.size());
    }
    if (!saltar.empty() && p.ps && std::find(saltar.begin(), saltar.end(), p.ps->numero) != saltar.end()) {
      return true;
    }
  }
  // fc3-switch 2026-11-08 DIAGNOSTIC (exposure hunt): the first 64 PS float constants of the lighting shaders matched
  // against the PC RenderDoc capture (store_frame8700, work/pc-ref-20261107/store-ps-map.json), once per shader per
  // 2000 frames, to compare the CPU-set light/exposure constants with the PC build's.
  if (p.ps && p.registros) {
    const uint32_t n = p.ps->numero;
    // fc3-switch 2026-11-09 DIAGNOSTIC (fc3_volcado_al_ps / fc3_volcado_al_ps_cada): frame dumps when a given PS
    // is drawn (the first time, then every N frames, 3 at most): a scene-locked trigger, unlike the seconds-based one.
    {
      static const int32_t ps_pedido = REXCVAR_GET(fc3_volcado_al_ps);
      static uint64_t proximo = 0;
      static uint32_t hechos = 0;
      if (ps_pedido >= 0 && n == uint32_t(ps_pedido) && hechos < 3 && fotograma_ >= proximo &&
          fotograma_ >= uint64_t(std::max(REXCVAR_GET(fc3_volcado_al_ps_desde), 0))) {
        ++hechos;
        proximo = fotograma_ + uint64_t(std::max(REXCVAR_GET(fc3_volcado_al_ps_cada), 1));
        PedirVolcado();
        REXLOG_INFO("[gpu] frame dump {} requested by PS {} at frame {}", hechos, n, fotograma_);
      }
    }
    // 2026-11-09: + PS 467 (Jason's arm, black albedo), every 40 frames, 30 times.
    if (const int32_t pedido = REXCVAR_GET(fc3_log_tex_ps); pedido >= 0 && n == uint32_t(pedido)) {
      static uint32_t veces = 0;
      static uint64_t ultimo = 0;
      if (veces < 30 && (!ultimo || fotograma_ - ultimo >= 40)) {
        ultimo = fotograma_;
        ++veces;
        for (const SamplerShader& sm : p.ps->samplers) {
          const uint32_t* f = p.registros + kRegFetch + uint32_t(sm.registro) * 6;
          REXLOG_INFO("[pstex] frame {} PS {} s{} tipo {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", fotograma_,
                      n, sm.registro, sm.tipo, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
      }
    }
    static uint32_t veces_467 = 0;
    if (n == 467 && veces_467 < 30) {
      static uint64_t ultimo_467 = 0;
      if (!ultimo_467 || fotograma_ - ultimo_467 >= 40) {
        ultimo_467 = fotograma_;
        ++veces_467;
        std::string s;
        for (uint32_t k = 0; k < 96 * 4; ++k) {
          float v;
          std::memcpy(&v, p.registros + kRegConstantesPs + k, 4);
          s += fmt::format("{}{:.6g}", k ? (k % 4 ? "," : " | ") : "", v);
        }
        REXLOG_INFO("[psconst] frame {} PS {} c0..c95: {}", fotograma_, n, s);
        // 2026-11-09: + the texture fetch constant of each of its samplers (which one is the 4x4 s2 on the Switch).
        for (const SamplerShader& sm : p.ps->samplers) {
          const uint32_t* f = p.registros + kRegFetch + uint32_t(sm.registro) * 6;
          REXLOG_INFO("[ps467tex] frame {} s{} tipo {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", fotograma_,
                      sm.registro, sm.tipo, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
      }
    }
    if (n == 583 || n == 1811 || n == 1142 || n == 1160 || n == 1396 || n == 15 || n == 2210 || n == 912) {
      static std::unordered_map<uint32_t, uint64_t> ultimo;
      auto it = ultimo.find(n);
      if (fotograma_ > 4000 && (it == ultimo.end() || fotograma_ - it->second >= 2000)) {
        ultimo[n] = fotograma_;
        std::string s;
        for (uint32_t k = 0; k < 256 * 4; ++k) {
          float v;
          std::memcpy(&v, p.registros + kRegConstantesPs + k, 4);
          s += fmt::format("{}{:.6g}", k ? (k % 4 ? "," : " | ") : "", v);
        }
        REXLOG_INFO("[psconst] frame {} PS {} c0..c255: {}", fotograma_, n, s);
      }
    }
  }
  vigia_dibujos_.fetch_add(1, std::memory_order_relaxed);
  vigia_vs_.store(p.vs->numero, std::memory_order_relaxed);
  vigia_ps_.store(p.ps ? p.ps->numero : 0, std::memory_order_relaxed);
  const uint64_t inicio_ns = deko::AhoraNs();
  const uint64_t espera_inicio_ns = nucleo_.NsEsperandoValla();  // waits for the GPU are not recording time
  // Stage times (ns_etapas_), without the waits for the GPU that may happen inside a stage.
  uint64_t marca_etapa = inicio_ns, espera_etapa = espera_inicio_ns;
  auto cortar_etapa = [&](uint32_t etapa) {
    const uint64_t ahora = deko::AhoraNs(), espera = nucleo_.NsEsperandoValla();
    ns_etapas_[etapa] += (ahora - marca_etapa) - (espera - espera_etapa);
    marca_etapa = ahora;
    espera_etapa = espera;
  };
  // nfsmw_dk_envio_parcial: what the frame recorded so far goes to the GPU now, so it draws while this thread keeps
  // recording (one submission per frame left it idle until the frame closed: 2-10 ms a frame in Dead Space 3's
  // fights). Everything is bound again afterwards. From gears2-nx's sdk-patches/0039.
  if (const int32_t cada = REXCVAR_GET(nfsmw_dk_envio_parcial);
      cada > 0 && ++dibujos_sin_envio_ >= uint32_t(cada) && nucleo_.TrabajoAbierto()) {
    dibujos_sin_envio_ = 0;
    nucleo_.EnviarParcial();
    OlvidarEstado();
    g_.pase_valido = false;
  }
  // A guest wait for the GPU: the game may rewrite vertex memory it already drew from (DedupeVertices).
  if (const uint32_t sinc = g_sincronizaciones_anillo.load(std::memory_order_relaxed); sinc != dedupe_sinc_vista_) {
    dedupe_sinc_vista_ = sinc;
    dedupe_.Olvidar();
    geometria_.Olvidar();
  }
  // --- Primitive
  const uint32_t modo_edram = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
  if (modo_edram != uint32_t(xenos::EdramMode::kColorDepth) && modo_edram != 5) {
    return Rechazar(1, "modo EDRAM sin color ni profundidad");
  }
  // Mode 5 (depth only): the Xenos does not run the pixel shader.
  const EntradaShader* ps = modo_edram == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
  if (!ps && modo_edram == uint32_t(xenos::EdramMode::kColorDepth)) {
    return false;
  }
  if (ps && ps->spirv.empty()) {
    return false;
  }
  const uint32_t iniciador = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
  const uint32_t tipo = iniciador & 0x3F;
  const uint32_t fuente = (iniciador >> 6) & 0x3;
  const bool indices32 = (iniciador >> 11) & 0x1;
  const uint32_t cuenta = iniciador >> 16;
  if (!cuenta) {
    return true;
  }
  DkPrimitive primitiva;
  bool admite_reinicio = false;
  bool rectangulos = false;
  switch (tipo) {
    case 2:
      primitiva = DkPrimitive_Lines;
      break;
    case 3:
      primitiva = DkPrimitive_LineStrip;
      admite_reinicio = true;
      break;
    case 4:
      primitiva = DkPrimitive_Triangles;
      break;
    case 5:
      primitiva = DkPrimitive_TriangleFan;
      admite_reinicio = true;
      break;
    case 6:
      primitiva = DkPrimitive_TriangleStrip;
      admite_reinicio = true;
      break;
    case 13:
      primitiva = DkPrimitive_Quads;  // native (the Vulkan backend makes triangle lists of them)
      break;
    case 8:
      // fc3-switch: rectangle list, through gs_rectangulos_ (destinos.cpp).
      if (!gs_rectangulos_valido_) {
        return Rechazar(108, "tipo de primitiva todavia no soportado");
      }
      primitiva = DkPrimitive_Triangles;
      rectangulos = true;
      break;
    case 1:
      // Point list: Dead Space 2 reduces the scene's luminance into its 64x8 exposure target with points. Their size
      // is set before the draw (PA_SU_POINT_SIZE).
      primitiva = DkPrimitive_Points;
      break;
    default:
      return Rechazar(100 + tipo, "tipo de primitiva todavia no soportado");
  }
  if (fuente == uint32_t(xenos::SourceSelect::kImmediate)) {
    return Rechazar(2, "indices inmediatos");
  }
  const bool sin_recorte = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
  const uint32_t modo_sc = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  if (((modo_sc >> 3) & 0x3) == 2 && ((modo_sc >> 5) & 0x7) != 2) {
    return Rechazar(4, "poligonos dibujados como puntos o lineas");
  }
  const EntradaVerticesDk* entrada = EntradaDe(p);
  if (!entrada) {
    return false;
  }

  // --- Render targets and what can be skipped
  const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  const uint32_t mascara_registro =
      modo_edram == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
  static constexpr uint32_t kInfoColor[4] = {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                                             gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
  static constexpr uint32_t kMezcla[4] = {gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
                                          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
  uint64_t claves[5] = {};
  uint32_t mascaras = 0;
  bool hay_destino = false;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t mascara = (mascara_registro >> (i * 4)) & 0xF;
    if (!mascara || !ps || !((ps->salidas >> i) & 0x1)) {
      continue;
    }
    const uint32_t info = r[kInfoColor[i]];
    const uint32_t formato = (info >> 16) & 0xF;
    if (DkImageFormat host; !FormatoDestinoColorDk(formato, host)) {
      return Rechazar(200 + formato, "formato de destino de color todavia no soportado");
    }
    claves[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(formato) << 16) | pitch;
    mascaras |= mascara << (i * 4);
    hay_destino = true;
  }
  const uint32_t control_profundidad = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
  // Draws that cannot change a pixel (nfsmw_nativo_saltar_invisibles; the Vulkan backend explains both cases).
  const bool saltar_invisibles = REXCVAR_GET(nfsmw_nativo_saltar_invisibles);  // read each time: the game can switch it (ds2 safe mode)
  if (saltar_invisibles && mascaras) {
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    const bool alfa_nunca = ((control_color >> 3) & 0x1) && (control_color & 0x7) == 0;
    uint32_t mascaras_utiles = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mascara_i = (mascaras >> (i * 4)) & 0xF;
      if (!mascara_i) {
        continue;
      }
      const uint32_t m = r[kMezcla[i]] & 0x1FFF1FFF;
      const bool copia_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
      const bool copia_alfa = ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
      if (((mascara_i & 0x7) != 0 && !copia_color) || ((mascara_i & 0x8) != 0 && !copia_alfa)) {
        mascaras_utiles |= mascara_i << (i * 4);
      }
    }
    if (!mascaras_utiles || alfa_nunca) {
      const bool deja_rastro =
          !alfa_nunca && ((((control_profundidad >> 1) & 0x1) && ((control_profundidad >> 2) & 0x1)) ||
                          (control_profundidad & 0x1));
      if (!deja_rastro) {
        return true;
      }
    }
  }
  // No color to write: the pixel shader only matters if it can discard or writes depth.
  if (!mascaras && ps) {
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    const bool prueba_alfa = ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7;
    const bool sin_ps_sin_color = REXCVAR_GET(nfsmw_nativo_sin_ps_sin_color);  // read each time: the game can switch it (ds2 safe mode)
    if (!prueba_alfa && !ps->descarta && !(ps->salidas & 0x10)) {
      if (sin_ps_sin_color) {
        ps = nullptr;
      }
    } else if (REXCVAR_GET(nfsmw_sombras_sin_vegetacion) && REXCVAR_GET(nfsmw_nativo_ps_solo_alfa)) {
      // Only the alpha test is left (vegetation in the shadow map): not drawn (nfsmw_sombras_sin_vegetacion).
      return true;
    }
  }
  if (control_profundidad & 0x3) {  // stencil or z
    const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
    claves[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) | (uint64_t((info >> 16) & 0x1) << 16) | pitch;
    hay_destino = true;
  }
  if (!hay_destino || !pitch) {
    return true;  // writes nothing visible
  }
  const bool destino_sombras = pitch >= 1600 && claves[4] && !claves[0] && !claves[1] && !claves[2] && !claves[3];

  // --- Depth bias (GetPreferredFacePolygonOffset and command_processor.cpp's scale, as the Vulkan backend)
  const uint32_t desplazamiento = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  const bool reinicio = admite_reinicio && ((modo_sc >> 21) & 0x1);
  float escala_sesgo = 0.0f, desplazamiento_sesgo = 0.0f;
  if (tipo >= 4) {
    if (((modo_sc >> 11) & 0x1) && !(modo_sc & 0x1)) {
      escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    if (((modo_sc >> 12) & 0x1) && !((modo_sc >> 1) & 0x1) && escala_sesgo == 0.0f && desplazamiento_sesgo == 0.0f) {
      escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
      desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
    }
  } else if ((modo_sc >> 13) & 0x1) {
    escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
    desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
  }
  float sesgo[2] = {desplazamiento_sesgo * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                                ? float(uint32_t(1) << 24)
                                                : float((uint32_t(1) << 24) - 1)),
                    escala_sesgo * (1.0f / 16.0f)};
  // A depth-only target as wide as a shadow map: what bias the game draws it with, once per width (to check).
  const bool solo_z = claves[4] && !claves[0] && !claves[1] && !claves[2] && !claves[3];
  if (solo_z && tipo >= 4 && pitch >= 512 && sesgos_vistos_.insert(pitch).second) {
    REXLOG_INFO("[deko3d] solo profundidad, ancho {}: PA_SU_SC_MODE_CNTL {:08X}, sesgo delante {} / {}, detras {} / {}",
                pitch, modo_sc, Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]),
                Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]),
                Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]),
                Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]));
  }
  const bool sombras_sesgo =
      destino_sombras || (solo_z && pitch >= uint32_t(REXCVAR_GET(nfsmw_dk_sombras_sesgo_pitch)));
  if (sombras_sesgo && tipo >= 4 && sesgo[0] == 0.0f && sesgo[1] == 0.0f) {
    sesgo[0] = float(REXCVAR_GET(nfsmw_nativo_sombras_sesgo_constante)) * 1000.0f;
    sesgo[1] = float(REXCVAR_GET(nfsmw_nativo_sombras_sesgo_pendiente)) * 0.1f;
  }
  // A depth prepass and a color pass over the same surface (Gears of War 2: depth only, then the base pass with
  // GEQUAL and its own vertex shader). On the Xbox 360 both give the same depth bit for bit; here two separately
  // compiled vertex shaders may differ in the last bit and the color pass fails where it should pass. Pulled 4 units
  // towards passing (nfsmw_dk_sesgo_igualdad), only when the game set no bias of its own.
  // Only passes that test depth without writing it: Dead Space 3's G-buffer pass is a color pass that writes depth
  // with LEQUAL too, and pulling it as well cancelled the pull of its material pass (which re-draws the same geometry
  // with LEQUAL and no depth write, through another vertex shader): half its pixels failed and the scene behind showed
  // through (a "transparent" player, holes in the ground, untextured mountains).
  if (REXCVAR_GET(nfsmw_dk_sesgo_igualdad) && tipo >= 4 && modo_edram == uint32_t(xenos::EdramMode::kColorDepth) &&
      sesgo[0] == 0.0f && sesgo[1] == 0.0f && (control_profundidad & 0x2) && !(control_profundidad & 0x4)) {
    const uint32_t funcion_z = (control_profundidad >> 4) & 0x7;
    if (funcion_z == 6) {
      sesgo[0] = 4.0f;  // GEQUAL
    } else if (funcion_z == 3) {
      sesgo[0] = -4.0f;  // LEQUAL
    }
  }
  const bool con_sesgo = sesgo[0] != 0.0f || sesgo[1] != 0.0f;

  // --- Vertex range and indices
  uint32_t vmin = UINT32_MAX;
  uint32_t vmax = 0;
  bool con_indices = false;
  bool indices_de_16 = false;
  uint32_t n_indices = 0;
  // The indices are either in the geometry store already (indices_almacen, with their range), or converted straight
  // into the ring at the upload below, in one pass: here only their range is read (the vertex ranges and the upload
  // space depend on it, and the ring slot may still change before the upload).
  const uint8_t* indices_datos = nullptr;
  xenos::Endian indices_orden = xenos::Endian::kNone;
  uint32_t indice_reinicio = 0;
  deko::AlmacenGeometria::Resultado indices_almacen{};
  if (fuente == uint32_t(xenos::SourceSelect::kDMA)) {
    const uint32_t tamano = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
    const auto orden = static_cast<xenos::Endian>(tamano >> 30);
    const uint32_t bytes = indices32 ? 4 : 2;
    const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
    if (cuenta > (tamano & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(cuenta) * bytes > kMemoriaFisica) {
      return Rechazar(7, "indices fuera de su bufer");
    }
    const uint8_t* datos = memoria_->TranslatePhysical(base & 0x1FFFFFFF);
    if (!indices32 && (base & 2) && (orden == xenos::Endian::k8in32 || orden == xenos::Endian::k16in32)) {
      // fc3-switch DIAGNOSTIC: draws whose index pairing fc3_dk_indices_alineados changes.
      static uint64_t desalineados = 0;
      ++desalineados;
      if (desalineados <= 8 || (desalineados & (desalineados - 1)) == 0) {
        REXLOG_INFO("[deko3d] indices de 16 bits a 2 mod 4 con orden {} (base {:08X}, {} indices): {} dibujos asi; "
                    "fc3_dk_indices_alineados={}",
                    uint32_t(orden), base, cuenta, desalineados, REXCVAR_GET(fc3_dk_indices_alineados));
      }
    }
    indices_datos = datos;
    indices_orden = orden;
    indice_reinicio = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
    // The usual case: 16 bits without restart or offset, uploaded as 16 bits. The rest as 32 bits.
    indices_de_16 = !indices32 && !reinicio && !desplazamiento &&
                    (orden == xenos::Endian::k8in16 || orden == xenos::Endian::kNone);
    const bool girar = orden == xenos::Endian::k8in16;
    if (geometria_.activo()) {
      // Everything the conversion depends on (bit 63: indices; the vertex modes are the byte order alone).
      const uint64_t modo = (uint64_t(1) << 63) | uint64_t(indices32) | (uint64_t(orden) << 1) |
                            (uint64_t(reinicio) << 3) | (uint64_t(desplazamiento) << 4) |
                            (uint64_t(reinicio ? indice_reinicio : 0) << 28);
      indices_almacen = geometria_.Buscar(
          datos, cuenta * bytes, modo, cuenta * (indices_de_16 ? 2u : 4u),
          [&](uint8_t* destino, uint32_t& minimo, uint32_t& maximo) {
            if (indices_de_16) {
              IndicesDe16<true>(datos, cuenta, reinterpret_cast<uint16_t*>(destino), girar, minimo, maximo);
            } else {
              IndicesDe32<true>(datos, cuenta, indices32, orden, reinicio, indice_reinicio, desplazamiento,
                                reinterpret_cast<uint32_t*>(destino), minimo, maximo);
            }
          });
    }
    if (indices_almacen.gpu) {
      vmin = indices_almacen.vmin;
      vmax = indices_almacen.vmax;
    } else if (indices_de_16) {
      IndicesDe16<false>(datos, cuenta, nullptr, girar, vmin, vmax);
    } else {
      IndicesDe32<false>(datos, cuenta, indices32, orden, reinicio, indice_reinicio, desplazamiento, nullptr, vmin,
                         vmax);
    }
    if (vmin > vmax) {
      return true;  // resets only
    }
    con_indices = true;
    n_indices = cuenta;
  } else {
    vmin = desplazamiento;
    vmax = desplazamiento + cuenta - 1;
  }
  if (vmax - vmin >= kMaxVerticesPorDibujo) {
    return Rechazar(8, "rango de vertices demasiado grande");
  }
  const uint32_t vertices = vmax - vmin + 1;
  // fc3-switch 2026-11-12 DIAGNOSTIC: the tile-lighting quads of VS 1417 (32_32_32_FLOAT positions, stride 16, first
  // binding), every 600th frame: per draw its PS, vertex range, raw first words and each 4-vertex group's bbox.
  if (p.vs->numero == 1417 && !entrada->enlaces.empty() && (fotograma_ % 600) == 0) {
    const EntradaVerticesDk::Enlace& e0 = entrada->enlaces[0];
    const uint32_t d0 = r[kRegFetch + e0.ranura * 2], d1 = r[kRegFetch + e0.ranura * 2 + 1];
    const uint64_t dir = uint64_t(d0 & 0x1FFFFFFC);
    const uint32_t zancada = e0.zancada ? e0.zancada : 16;
    const uint8_t* g = memoria_->TranslatePhysical(0);
    auto leer = [&](uint32_t v, uint32_t k) {
      const uint64_t a = dir + uint64_t(v) * zancada + k * 4;
      if (a + 4 > 0x20000000) return 0.0f;
      uint32_t w;
      std::memcpy(&w, g + a, 4);
      w = xenos::GpuSwap(w, static_cast<xenos::Endian>(d1 & 0x3));
      float f;
      std::memcpy(&f, &w, 4);
      return f;
    };
    std::string q;
    const uint32_t n = std::min<uint32_t>(vertices, 512);
    for (uint32_t v = 0; v < n; ++v) {  // every vertex in screen pixels (NDC -> 1280x704)
      const float x = leer(vmin + v, 0), y = leer(vmin + v, 1);
      q += fmt::format(" {},{}", int((x + 1.0f) * 640.0f + 0.5f), int((1.0f - y) * 352.0f + 0.5f));
    }
    REXLOG_INFO("[quads] frame {} ps {} vmin {} vmax {} endian {} stride {} fetch {:08X} {:08X} v0 {:.3f},{:.3f},{:.3f} "
                "groups:{}",
                fotograma_, p.ps ? int(p.ps->numero) : -1, vmin, vmax, d1 & 3, zancada, d0, d1, leer(vmin, 0),
                leer(vmin, 1), leer(vmin, 2), q);
  }

  // Bytes of each vertex binding in guest memory.
  struct Origen {
    const uint8_t* datos;
    xenos::Endian orden;
    uint32_t bytes;
  };
  std::array<Origen, 16> origenes{};
  uint32_t bytes_vertices = 0;
  for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
    const EntradaVerticesDk::Enlace& enlace = entrada->enlaces[b];
    const uint32_t d0 = r[kRegFetch + enlace.ranura * 2];
    const uint32_t d1 = r[kRegFetch + enlace.ranura * 2 + 1];
    if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
      return Rechazar(10, "fetch constant de vertices invalida");
    }
    const uint64_t direccion = uint64_t(d0 & 0x1FFFFFFC);
    const uint64_t disponibles = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
    // fc3_dk_fetch_indice_cero: the stream is read at index 0 by every vertex (bound with stride 0): record 0 only.
    const uint64_t inicio = enlace.indice_cero ? 0 : uint64_t(vmin) * enlace.zancada;
    // fc3_dk_stride0 (gears2-nx 0034): stride 0 is one element for every vertex (up to 64 bytes: every
    // attribute's offset fits).
    uint64_t necesarios = enlace.indice_cero ? std::min<uint64_t>(disponibles, enlace.zancada) & ~uint64_t(3)
                          : enlace.zancada   ? uint64_t(vertices) * enlace.zancada
                                             : std::min<uint64_t>(disponibles, 64);
    if (enlace.indice_cero) {
      ++indice_cero_dibujos_;
      vigia_fetch0_dir_.store(direccion, std::memory_order_relaxed);
      vigia_fetch0_bytes_.store(necesarios, std::memory_order_relaxed);
      vigia_fetch0_dibujo_.store(vigia_dibujos_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      if (vigia_fetch0_vs_.insert(p.vs->numero).second) {
        // fc3-switch DIAGNOSTIC: one line per VS: the binding and whether it is sane (aligned, non-empty, inside the
        // 4 KB page and the fetch constant's size), and the attributes read from it.
        std::string atributos;
        for (uint32_t u = 0; u < entrada->n_atributos; ++u) {
          const DkVtxAttribState& a = entrada->atributos[u];
          if (!a.isFixed && a.bufferId == b) {
            atributos += fmt::format(" loc{}:off{}/size{}/type{}", u, uint32_t(a.offset), uint32_t(a.size),
                                     uint32_t(a.type));
          }
        }
        REXLOG_INFO("[fetch0] vs={} slot={} addr={:08X} available={} stride={} record={} endian={} aligned4={} "
                    "aligned16={} page_end={} past_page={} past_available={}{}",
                    p.vs->numero, enlace.ranura, direccion, disponibles, enlace.zancada, necesarios, d1 & 3,
                    (direccion & 3) == 0, (direccion & 15) == 0, (direccion & 0xFFF) + disponibles,
                    (direccion & 0xFFF) + enlace.zancada > 0x1000, enlace.zancada > disponibles, atributos);
      }
    } else if (enlace.zancada && inicio + necesarios > disponibles) {
      Avisar(11, "vertices mas alla del final de su bufer: se recorta");
      // fc3-switch DIAGNOSTIC: how often, and the first few.
      ++recortes_11_;
      if (recortes_11_ <= 16 || (recortes_11_ & (recortes_11_ - 1)) == 0) {
        REXLOG_WARN("[deko3d] causa 11 detalle n={} vs={} slot={} fetch={:08X}/{:08X} stride={} min={} max={} "
                    "available={} start={}",
                    recortes_11_, p.vs->numero, enlace.ranura, d0, d1, enlace.zancada, vmin, vmax, disponibles, inicio);
      }
      necesarios = disponibles > inicio ? (disponibles - inicio) / enlace.zancada * enlace.zancada : 0;
    }
    if (!necesarios || direccion + inicio + necesarios > kMemoriaFisica) {
      // Keep the guest fetch and draw identity: a one-time cause warning is
      // insufficient to distinguish invalid fetches from an index/range bug.
      static uint32_t detalles_fuera = 0;
      if (detalles_fuera < 32) {
        ++detalles_fuera;
        REXLOG_WARN("[deko3d] causa 12 detalle frame={} vs={} vs_hash={:016X} ps={} ps_hash={:016X} "
                    "slot={} fetch={:08X}/{:08X} stride={} min={} max={} available={} start={} bytes={} "
                    "primitive={} count={} indexed={} target={:03X} pitch={}",
                    fotograma_, p.vs->numero, p.vs->huella, ps ? ps->numero : 0,
                    ps ? ps->huella : uint64_t(0), enlace.ranura, d0, d1, enlace.zancada, vmin, vmax,
                    disponibles, inicio, necesarios, tipo, cuenta, con_indices, uint32_t(claves[0] >> 24) & 0xFFF,
                    pitch);
      }
      return Rechazar(12, "vertices fuera de la memoria");
    }
    origenes[b] = {memoria_->TranslatePhysical(uint32_t(direccion + inicio)), static_cast<xenos::Endian>(d1 & 0x3),
                   uint32_t(necesarios)};
    bytes_vertices += (uint32_t(necesarios) + 15) & ~15u;
  }

  cortar_etapa(0);
  // --- Textures and samplers (they may move on to the next work slot: before anything this draw records)
  AsegurarTrabajo();
  // nfsmw_nativo_sombra_minimo: the texture with the cars, which is sampled paired with the world's (0 = none).
  const uint32_t dir_coches = ps ? DireccionSombraCoches() : 0;
  bool sombra_minimo_dibujo = false;
  uint32_t compartidas[kPalabrasCompartidas] = {};
  std::array<DkResHandle, 32> texturas{};
  uint32_t n_texturas = 0;
  uint32_t tex0_ancho = 0, tex0_alto = 0, tex0_formato = 0, tex0_sampler = 0;  // for the GPU profile
  // The previous draw's answer, when this one has the same pixel shader and the same fetch words for its samplers
  // and nothing about textures changed since (same work, same texture generation, every answer still valid, no
  // read of the cars' shadow texture, whose reads feed the shadow minimum's guard).
  bool texturas_reusadas = false;
  if (ps && texturas_previas_.ps == ps && texturas_previas_.trabajo == trabajo_ &&
      texturas_previas_.generacion == generacion_texturas_ && fotograma_ <= texturas_previas_.valido_hasta &&
      !texturas_previas_.toca_coches && texturas_previas_.dir_coches == dir_coches) {
    bool iguales = true;
    for (const SamplerShader& sampler : ps->samplers) {
      if (sampler.registro < 16 &&
          std::memcmp(texturas_previas_.fetch.data() + sampler.registro * 6, r + kRegFetch + uint32_t(sampler.registro) * 6,
                      sizeof(uint32_t) * 6) != 0) {
        iguales = false;
        break;
      }
    }
    if (iguales) {
      texturas = texturas_previas_.texturas;
      n_texturas = texturas_previas_.n_texturas;
      std::memcpy(compartidas + kPalabraInvTamano, texturas_previas_.inversos.data(), sizeof(texturas_previas_.inversos));
      std::memcpy(compartidas + 124, texturas_previas_.sesgos.data(), sizeof(texturas_previas_.sesgos));
      tex0_ancho = texturas_previas_.tex0[0];
      tex0_alto = texturas_previas_.tex0[1];
      tex0_formato = texturas_previas_.tex0[2];
      tex0_sampler = texturas_previas_.tex0[3];
      texturas_reusadas = true;
      ++texturas_reusadas_;
    }
  }
  uint64_t valido_minimo = UINT64_MAX;
  bool toca_coches = false;
  if (ps && !texturas_reusadas) {
    for (const SamplerShader& sampler : ps->samplers) {
      if (sampler.registro >= 16) {
        continue;
      }
      const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.registro) * 6;
      CacheFetch& cache = cache_fetch_[ClaveFetch(fetch) & (cache_fetch_.size() - 1)];
      if (!(fotograma_ <= cache.valido_hasta && cache.generacion == generacion_texturas_ &&
            std::memcmp(cache.fetch.data(), fetch, sizeof(cache.fetch)) == 0)) {
        bool puntual = false;
        uint32_t ancho = 0, alto = 0;
        uint64_t valido_hasta = fotograma_;
        uint32_t sesgo = 0;
        const uint32_t ranura = PrepararTextura(fetch, sampler.tipo, ancho, alto, puntual, valido_hasta, sesgo);
        cache.valido_hasta = valido_hasta;
        cache.sampler = SamplerDe(fetch, puntual);
        cache.ranura = ranura;
        cache.ancho = ancho;
        cache.alto = alto;
        cache.sesgo_sin_convertir = sesgo;
        cache.fotograma = fotograma_;
        cache.generacion = generacion_texturas_;
        std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      }
      valido_minimo = std::min(valido_minimo, cache.valido_hasta);
      const DkResHandle h = dkMakeTextureHandle(cache.ranura, cache.sampler);
      if (!tex0_ancho) {
        tex0_ancho = cache.ancho;
        tex0_alto = cache.alto;
        tex0_formato = fetch[1] & 0x3F;
        tex0_sampler = cache.sampler;
      }
      texturas[sampler.registro] = h;
      if (REXCVAR_GET(fc3_biased_texture_experiment)) {
        compartidas[124 + sampler.registro / 8] |= cache.sesgo_sin_convertir << ((sampler.registro % 8) * 4);
        // Report each shader/sampler pair once to find other materials that
        // need a matching shader, without flooding a normal gameplay trace.
        static std::array<uint16_t, 4096> sesgos_informados{};
        static uint32_t total_sesgos_informados = 0;
        const uint16_t bit = uint16_t(1u << sampler.registro);
        if (total_sesgos_informados < 256 && ps->numero < sesgos_informados.size() && cache.sesgo_sin_convertir &&
            !(sesgos_informados[ps->numero] & bit)) {
          sesgos_informados[ps->numero] |= bit;
          ++total_sesgos_informados;
          REXLOG_INFO("[fc3-bias] ps={} sampler={} mask={:X} fetch={:08X}/{:08X}/{:08X}", ps->numero,
                      sampler.registro, cache.sesgo_sin_convertir, fetch[0], fetch[1], fetch[3]);
        }
      }
      n_texturas = std::max<uint32_t>(n_texturas, sampler.registro + 1u);
      // tfetch2DSombraMin's pair (binding 16 + R): the world's shadow map when the texture with the cars holds the
      // cars only, the texture itself otherwise (the minimum with itself is the same texel).
      const bool capaz = ps->sombra_minimo && sampler.mapa_sombras &&
                         ((fetch[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked);
      DkResHandle pareja_h = h;
      if (dir_coches && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          (((fetch[1] >> 12) << 12) & 0x1FFFFFFF) == dir_coches) {
        toca_coches = true;
        // Every read of it is reported, also from shaders that cannot take the minimum: that is the guard.
        if (const ImagenDk* pareja = CompaneraSombraCoches(capaz, ps->numero); pareja && capaz) {
          pareja_h = dkMakeTextureHandle(VistaDe(*pareja, (fetch[3] >> 1) & 0xFFF, kSwizzleParejaSombra), cache.sampler);
          sombra_minimo_dibujo = true;
        }
      }
      if (capaz) {
        texturas[16 + sampler.registro] = pareja_h;
        n_texturas = std::max<uint32_t>(n_texturas, 16u + sampler.registro + 1u);
      }
      const float inv[2] = {cache.ancho ? 1.0f / float(cache.ancho) : 0.0f,
                            cache.alto ? 1.0f / float(cache.alto) : 0.0f};
      std::memcpy(compartidas + kPalabraInvTamano + sampler.registro * 2, inv, sizeof(inv));
    }
    texturas_previas_.ps = ps;
    texturas_previas_.trabajo = trabajo_;
    texturas_previas_.generacion = generacion_texturas_;
    texturas_previas_.valido_hasta = valido_minimo;
    texturas_previas_.toca_coches = toca_coches || sombra_minimo_dibujo;
    texturas_previas_.dir_coches = dir_coches;
    std::memcpy(texturas_previas_.fetch.data(), r + kRegFetch, sizeof(uint32_t) * 96);
    texturas_previas_.texturas = texturas;
    texturas_previas_.n_texturas = n_texturas;
    std::memcpy(texturas_previas_.inversos.data(), compartidas + kPalabraInvTamano, sizeof(texturas_previas_.inversos));
    std::memcpy(texturas_previas_.sesgos.data(), compartidas + 124, sizeof(texturas_previas_.sesgos));
    texturas_previas_.tex0 = {tex0_ancho, tex0_alto, tex0_formato, tex0_sampler};
  }

  cortar_etapa(1);
  // --- Upload space for everything this draw needs from here on, all in one slot
  const uint32_t bytes_indices =
      con_indices && !indices_almacen.gpu ? (indices_de_16 ? n_indices * 2 : n_indices * 4) : 0;
  const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constantes_bytes, kRegistrosConstantes * 4);
  const uint32_t bytes_ps = ps ? std::min<uint32_t>(ps->constantes_bytes, kRegistrosConstantes * 4) : 0;
  // Manual vertex fetches upload whole buffers as storage buffers, independently
  // of the regular vertex bindings counted in bytes_vertices above.
  uint64_t bytes_manuales = 0;
  if (!entrada->manuales.empty()) {
    for (uint32_t ranura : entrada->ranuras_manuales) {
      const uint32_t d0 = r[kRegFetch + ranura * 2];
      const uint32_t d1 = r[kRegFetch + ranura * 2 + 1];
      const uint64_t direccion = uint64_t(d0 & 0x1FFFFFFC);
      const uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex) || !bytes || bytes > (8u << 20) ||
          direccion + bytes > kMemoriaFisica) {
        return Rechazar(27, "bufer de vertices leido por el shader invalido o demasiado grande");
      }
      bytes_manuales += uint64_t(bytes) + 15;  // allocation alignment
    }
    bytes_manuales += 512 + DK_UNIFORM_BUF_ALIGNMENT - 1;  // up to 32 fetch descriptors
  }
  const uint64_t necesarios = uint64_t(bytes_vertices) + bytes_manuales + bytes_indices + 16 +
                              3 * DK_UNIFORM_BUF_ALIGNMENT + std::max(bytes_vs, kUboBytesVs) +
                              std::max(bytes_ps, kUboBytesPs) + 512;  // largest shared constants block
  const uint32_t rebanada = nucleo_.config().mb_subida << 20;
  if (necesarios > rebanada) {
    return Rechazar(13, "dibujo mayor que el anillo de subida");
  }
  if (nucleo_.SubidaLibre() < necesarios) {
    CambiarDeRanura();
  }

  // --- Render targets
  if (!g_.pase_valido || std::memcmp(g_.pase, claves, sizeof(claves)) != 0) {
    if (!EmpezarPase(claves, pitch)) {
      return false;
    }
  }
  // Pass setup can itself upload copy constants. If it consumed the space
  // checked above, start a fresh work slot and bind the pass again before
  // recording this draw's state or allocations.
  if (nucleo_.SubidaLibre() < necesarios) {
    CambiarDeRanura();
    if (!EmpezarPase(claves, pitch)) {
      return false;
    }
    if (nucleo_.SubidaLibre() < necesarios) {
      return Rechazar(13, "dibujo y preparacion de destino no caben en el anillo de subida");
    }
  }
  DkCmdBuf cmd = nucleo_.comandos();

  // --- Framing (cached while generacion_encuadre and the pass do not change)
  const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
  float viewport[6];
  float ndc[4];
  DkScissor tijera;
  uint32_t vacio;
  if (encuadre_generacion_ == p.generacion_encuadre && encuadre_pase_serie_ == g_.pase_serie) {
    std::memcpy(viewport, encuadre_viewport_, sizeof(viewport));
    std::memcpy(ndc, encuadre_ndc_, sizeof(ndc));
    tijera = encuadre_tijera_;
    vacio = encuadre_vacio_;
  } else {
    vacio = CalcularEncuadre(r, vte, sin_recorte, modo_sc, viewport, ndc, tijera);
    std::memcpy(encuadre_viewport_, viewport, sizeof(viewport));
    std::memcpy(encuadre_ndc_, ndc, sizeof(ndc));
    encuadre_tijera_ = tijera;
    encuadre_vacio_ = vacio;
    encuadre_generacion_ = p.generacion_encuadre;
    encuadre_pase_serie_ = g_.pase_serie;
  }
  if (vacio) {
    return true;  // empty viewport or scissor
  }
  // The useful area of the bound targets: nothing is drawn below the scissor. If this draw reaches rows the last
  // clear of a target left as they were, they are cleared first (nfsmw_dk_borrar_area_util).
  {
    const uint32_t hasta = tijera.y + tijera.height;
    for (uint32_t i = 0; i < 4; ++i) {
      if (ImagenDk* imagen = imagenes_pase_[i]) {
        CompletarBanda(*imagen, hasta, true, i);
      }
    }
    if (imagenes_pase_[4]) {
      CompletarBanda(*imagenes_pase_[4], hasta, true, 0);
    }
  }

  // --- Specialization and shared constants
  uint32_t especializacion = entrada->especializacion;
  static const bool inv_tamano = REXCVAR_GET(nfsmw_nativo_inv_tamano_tex);
  const bool pcf_barato = REXCVAR_GET(nfsmw_nativo_pcf_barato);  // read each time: the game can switch it (ds2 safe mode)
  if (inv_tamano) {
    especializacion |= kSpecInvTamanoTex;
  }
  if (pcf_barato) {
    especializacion |= kSpecPcfBarato;
  }
  if (sombra_minimo_dibujo) {
    especializacion |= kSpecSombraMinimo;
  }
  // nfsmw_nativo_sombra_minimo: a draw of the shadow map's car pass must leave in depth exactly the minimum of what was
  // there and of its fragments (no stencil and, if it writes Z, a NEVER, LESS or LEQUAL test).
  if (pase_coches_sombra_) {
    const uint32_t control_z = claves[4] ? control_profundidad : 0;
    const bool escribe_z = ((control_z >> 1) & 0x1) && ((control_z >> 2) & 0x1);
    const uint32_t funcion_z = (control_z >> 4) & 0x7;
    DibujoDeCochesSombra(!(control_z & 0x1) && (!escribe_z || funcion_z == 0 || funcion_z == 1 || funcion_z == 3),
                         control_z);
  }
  const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
  // fc3-switch 2026-11-09 DIAGNOSTIC (residual snow): which pixel shaders draw with alpha-to-mask (RB_COLORCONTROL bit
  // 4), which this backend ignores; each PS once, 96 at most, with the alpha test and the A2C offsets.
  if (ps && ((control_color >> 4) & 0x1)) {
    static std::array<uint8_t, 4096> a2c_vistos{};
    static uint32_t a2c_n = 0;
    if (a2c_n < 96 && ps->numero < a2c_vistos.size() && !a2c_vistos[ps->numero]) {
      a2c_vistos[ps->numero] = 1;
      ++a2c_n;
      REXLOG_INFO("[a2c] frame {} PS {} VS {} colorcontrol {:08X} (alpha test {} func {} ref {:.4g}) surface {:08X}",
                  fotograma_, ps->numero, p.vs->numero, control_color, (control_color >> 3) & 0x1, control_color & 0x7,
                  Flotante(r[gr::XE_GPU_REG_RB_ALPHA_REF]), r[gr::XE_GPU_REG_RB_SURFACE_INFO]);
    }
  }
  float umbral_alfa = 0.0f;
  uint32_t funcion_alfa = 7;  // always
  if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
    umbral_alfa = Flotante(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
    funcion_alfa = control_color & 0x7;
    especializacion |= 0x2 | ((funcion_alfa & 0x7u) << kSpecFuncionAlfaDesplazamiento);
  }
  // nfsmw_nativo_sin_desenfoque: the race's final composite without its radial blur (the Vulkan path's
  // composicion_sin_desenfoque; its texture side only feeds the lazy depth copies, which this backend has not).
  // The game classifies its own passes (sky, bright pass, composite); a game with no effects classifies none.
  const rex::x360::ClasePase clase = ps ? hooks_.Clasificar(*ps) : rex::x360::ClasePase{};
  if (clase.sin_desenfoque) {
    especializacion |= kSpecSinDesenfoque;
  }
  uint32_t variante_ps = clase.variante;
  // nfsmw_nativo_z_temprana, the Vulkan path's rule: a pixel shader that can discard (alpha test or its own kill)
  // makes the GPU shade before testing depth, which only matters if the draw writes depth or stencil. When it only
  // tests depth, testing first is exact and hidden fragments are not shaded. Not for the bright pass and the
  // composite (full-screen, no depth). Game occlusion queries are not measured by this backend, so there is no
  // count to protect.
  if (ps && claves[4] && mascaras && REXCVAR_GET(nfsmw_nativo_z_temprana)) {
    const bool prueba_z = (control_profundidad >> 1) & 0x1;
    const bool escribe_z = prueba_z && ((control_profundidad >> 2) & 0x1);
    const bool estencil = control_profundidad & 0x1;
    const bool ps_escribe_z = ps->salidas & 0x10;
    const bool tardio = (especializacion & 0x2) || ps->descarta || ps_escribe_z;
    if (prueba_z && tardio && !escribe_z && !ps_escribe_z && !estencil && !clase.pantalla_completa_sin_z) {
      variante_ps |= kVarianteZTemprana;
      ++dibujos_z_temprana_;
    }
  }
  compartidas[64] = (r[kRegBooleanos] & 0xFFFF) | ((r[kRegBooleanos + 4] & 0xFFFF) << 16);
  compartidas[65] = 0;  // g_SwappedTexcoords
  // fc3_dk_medio_pixel (DIAGNOSTIC, 2026-11-09): 0 = the D3D9 half-pixel shift as before, 1 = none, 2 = opposite sign.
  int32_t modo_medio = REXCVAR_GET(fc3_dk_medio_pixel);
  // fc3_dk_medio_pixel_alternar_s (DIAGNOSTIC): from that many seconds on, the mode above and 0 alternate every 20 s
  // (A/B in one run at the same pose); each switch is logged.
  if (const int32_t desde = REXCVAR_GET(fc3_dk_medio_pixel_alternar_s); desde > 0) {
    static const auto inicio = std::chrono::steady_clock::now();
    const int64_t s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio).count();
    const int32_t efectivo = s < desde ? 0 : (((s - desde) / 20) % 2 ? 0 : modo_medio);
    static int32_t anterior = -1;
    if (efectivo != anterior) {
      anterior = efectivo;
      REXLOG_INFO("[medio] half-pixel mode {} from second {} (frame {})", efectivo, s, fotograma_);
    }
    modo_medio = efectivo;
  }
  if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1) && modo_medio != 1) {  // PixelCenter::kD3DZero
    const float signo = modo_medio == 2 ? -1.0f : 1.0f;
    const float medio[2] = {signo / viewport[2], -signo / std::abs(viewport[3])};
    std::memcpy(&compartidas[66], medio, sizeof(medio));
  }
  std::memcpy(&compartidas[68], &umbral_alfa, sizeof(umbral_alfa));
  compartidas[69] = funcion_alfa;
  std::memcpy(&compartidas[70], ndc, sizeof(ndc));
  std::copy(entrada->remapeos.begin(), entrada->remapeos.end(), compartidas + 74);
  {
    const float escala = REXCVAR_GET(fc3_dk_blendindices_normalizados) ? entrada->escala_indices : 1.0f;
    std::memcpy(&compartidas[123], &escala, sizeof(escala));
  }
  if (entrada->mitades16 && REXCVAR_GET(fc3_dk_mitades_8in32)) {
    for (uint32_t u = 0; u < 16; ++u) {
      const uint32_t componentes = (entrada->mitades16 >> u) & 0x1 ? 2 : (entrada->mitades16 >> (u + 16)) & 0x1 ? 4 : 0;
      const DkVtxAttribState& a = entrada->atributos[u];
      if (!componentes || a.isFixed || a.bufferId >= entrada->enlaces.size()) {
        continue;
      }
      const uint32_t d1 = r[kRegFetch + entrada->enlaces[a.bufferId].ranura * 2 + 1];
      if (static_cast<xenos::Endian>(d1 & 0x3) == xenos::Endian::k8in32) {
        compartidas[74 + u] = RemapeoMitades(entrada->remapeos[u], componentes);
      }
    }
  }
  // g_IndiceBase (byte 488): the first vertex index of the draw. The draw starts its vertex buffers at vmin (base
  // vertex -vmin for indices; the index offset for auto-indexed draws), so SV_VertexID counts from it, and the vertex
  // shaders that read the Xenos vertex index (r0.x: Dead Space 2's particles) add it back.
  compartidas[122] = vmin;

  // --- The deferred sky (nfsmw_nativo_cielo_aplazado; the Vulkan path's criteria, explained there). The dome is the
  // sky's pixel shader, opaque, testing depth without writing it, no stencil, 480 indices, the first draw of the
  // scene's pass. It may be moved past a draw that is opaque and writes depth (the depth test then discards the sky
  // exactly where that draw painted); the first draw that is not, a pass change or a copy emits it first.
  bool opaco_en_todos = true;
  for (uint32_t i = 0; i < 4 && opaco_en_todos; ++i) {
    const uint32_t mascara_i = (mascaras >> (i * 4)) & 0xF;
    if (!mascara_i) {
      continue;
    }
    const uint32_t m = r[kMezcla[i]];
    const bool color_directo = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
    const bool alfa_directo = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
    if (((mascara_i & 0x7) && !color_directo) || ((mascara_i & 0x8) && !alfa_directo)) {
      opaco_en_todos = false;
    }
  }
  const bool prueba_z_dibujo = (control_profundidad >> 1) & 0x1;
  const bool escribe_z_dibujo = prueba_z_dibujo && ((control_profundidad >> 2) & 0x1) && claves[4];
  bool es_cielo = false;
  if (clase.cielo && mascaras) {
    const bool es_el_domo = CategoriaDeDestino(pitch, claves) == kGpuEscena && opaco_en_todos && prueba_z_dibujo &&
                            !escribe_z_dibujo && !(control_profundidad & 0x1) && !(ps->salidas & 0x10) &&
                            con_indices && n_indices == kIndicesDomoCielo && dibujos_en_pase_ == 0;
    if (es_el_domo && ++cielo_guardia_en_fotograma_ > 1 && cielo_guardia_ != kCieloDescartado) {
      cielo_guardia_ = kCieloDescartado;
      REXLOG_WARN("[deko3d] cielo: la guardia ve {} domos en el MISMO fotograma; no se aplaza nada en la sesion",
                  cielo_guardia_en_fotograma_);
    }
    const bool aplazar_cielo = REXCVAR_GET(nfsmw_nativo_cielo_aplazado);  // read each time: the game can switch it (ds2 safe mode)
    es_cielo = aplazar_cielo && es_el_domo && cielo_guardia_ == kCieloAplazando;
  }
  if (cielo_pendiente_ && (es_cielo || !(opaco_en_todos && escribe_z_dibujo))) {
    EmitirCieloPendiente();  // before this draw binds anything
  }
  if (volcando_ && ps && ps->numero == 2284 && pitch == 1280 && imagenes_pase_[4] &&
      ((claves[4] >> 24) & 0xFFF) == 0x540 && REXCVAR_GET(fc3_trace_lighting_checkpoint) &&
      std::none_of(volcado_.begin(), volcado_.end(), [](const EntradaVolcado& e) { return e.base == 0xFC054002u; })) {
    const ImagenDk& z = *imagenes_pase_[4];
    VolcarResuelta(z, 0xFC054002u, 23);
    REXLOG_INFO("[lighting-checkpoint] pre-PS2284 frame={} indices={} z={} depthctl={:08X} stencil={:08X} cleared={} used={} dirty={} pending={}",
                fotograma_, cuenta, z.serie, control_profundidad, r[gr::XE_GPU_REG_RB_STENCILREFMASK],
                z.alto_borrado, z.alto_usado, z.banda_sucia, z.borrado_pendiente);
  }
  AntesDe3D();  // a transfer recorded since the pass began (a cubemap's faces) is finished first

  // --- Shaders
  const DkShader* vs = ShaderDe(*p.vs, especializacion, ps ? InfoDe(*ps).interpoladores : 0u);
  const DkShader* fs = ps ? ShaderDe(*ps, especializacion, 0, variante_ps) : (fs_vacio_valido_ ? &fs_vacio_ : nullptr);
  if (!vs || !fs) {
    return Rechazar(60, "shader sin DKSH");
  }
  // fc3-switch 2026-11-08 DIAGNOSTIC: fc3_dk_color_falso (see its cvar).
  // fc3-switch 2026-11-12 DIAGNOSTIC: PS 760 (volumetric ambient) constants c72-c84 and its fetch constants s0-s5,
  // every 600th draw, to recompute its output offline (the flat 0.5 / 1.0 block question).
  // fc3-switch 2026-11-12 DIAGNOSTIC (lamp-house rectangle = PS 586 writes 1.0 in its light-volume rect, PC ~0.3-0.6):
  // the local-ambient chain PS 1064 / 840 / 586 / 1318: PS constants c0-c4 and c72-c84, RB_COLOR_INFO (format, exp
  // bias), RB_BLENDCONTROL0, color mask, every 300th occurrence of each.
  if (ps && (ps->numero == 586 || ps->numero == 840 || ps->numero == 1064 || ps->numero == 1318)) {
    static uint32_t veces_amb[4] = {};
    const int k = ps->numero == 586 ? 0 : ps->numero == 840 ? 1 : ps->numero == 1064 ? 2 : 3;
    if ((veces_amb[k]++ % 300) < 2) {
      auto f = [&](uint32_t reg) { float x; std::memcpy(&x, &r[reg], 4); return x; };
      std::string c;
      for (uint32_t i : {0u, 1u, 2u, 3u, 4u, 72u, 73u, 74u, 75u, 76u, 77u, 78u, 79u, 80u}) {
        const uint32_t b = 0x4400 + i * 4;
        c += fmt::format(" c{}={:.6g},{:.6g},{:.6g},{:.6g}", i, f(b), f(b + 1), f(b + 2), f(b + 3));
      }
      const uint32_t info = r[gr::XE_GPU_REG_RB_COLOR_INFO];
      REXLOG_INFO("[amb] frame {} ps {} vs {} colorinfo {:08X} (fmt {} expbias {}) blend0 {:08X} mask {:08X} depthctl {:08X}{}",
                  fotograma_, ps->numero, p.vs ? int(p.vs->numero) : -1, info, (info >> 16) & 0xF,
                  int32_t(info << 6) >> 26, r[gr::XE_GPU_REG_RB_BLENDCONTROL0], r[gr::XE_GPU_REG_RB_COLOR_MASK],
                  r[gr::XE_GPU_REG_RB_DEPTHCONTROL], c);
      if (ps->numero == 840) {
        // fc3-switch 2026-11-13 DIAGNOSTIC: the volume passes' raster state and VS constants (C4-C7 matrix, C36, C38
        // camera, C72-C75 volume transform), to see whether the +1 and -1 passes differ on Switch.
        std::string v;
        for (uint32_t i : {4u, 5u, 6u, 7u, 36u, 38u, 72u, 73u, 74u, 75u}) {
          const uint32_t b = 0x4000 + i * 4;
          v += fmt::format(" C{}={:.6g},{:.6g},{:.6g},{:.6g}", i, f(b), f(b + 1), f(b + 2), f(b + 3));
        }
        REXLOG_INFO("[amb840] frame {} blend0 {:08X} sc_mode {:08X} clip_cntl {:08X} vte {:08X}{}", fotograma_,
                    r[gr::XE_GPU_REG_RB_BLENDCONTROL0], r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL],
                    r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL], v);
      }
    }
  }
  if (ps && ps->numero == 760) {
    static uint32_t veces760 = 0;
    if ((veces760++ % 600) == 0 && veces760 < 30000) {
      auto f = [&](uint32_t reg) { float x; std::memcpy(&x, &r[reg], 4); return x; };
      std::string c;
      for (uint32_t i = 72; i <= 84; ++i) {
        const uint32_t b = 0x4400 + i * 4;
        c += fmt::format(" c{}={:.6g},{:.6g},{:.6g},{:.6g}", i, f(b), f(b + 1), f(b + 2), f(b + 3));
      }
      REXLOG_INFO("[ps760] frame {} draw{}", fotograma_, c);
      for (uint32_t s = 0; s < 6; ++s) {
        const uint32_t b = 0x4800 + s * 6;
        REXLOG_INFO("[ps760]   s{} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", s, r[b], r[b + 1], r[b + 2], r[b + 3],
                    r[b + 4], r[b + 5]);
      }
    }
  }
  // fc3_dk_color_falso_ps (DIAGNOSTIC): a palette colour per listed PS number, in any pass.
  if (ps && fs_falso_valido_) {
    static const std::string lista = REXCVAR_GET(fc3_dk_color_falso_ps);
    static std::vector<uint32_t> numeros;
    static bool todos = false, leida = false;
    if (!leida) {
      leida = true;
      todos = lista == "*";
      size_t i = 0;
      while (!todos && i < lista.size()) {
        const size_t j = lista.find(',', i);
        numeros.push_back(uint32_t(std::strtoul(lista.substr(i, j == std::string::npos ? std::string::npos : j - i).c_str(), nullptr, 10)));
        if (j == std::string::npos) break;
        i = j + 1;
      }
    }
    const bool ancho1280 = (r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF) == 1280;
    // "*" = scene draws only: no rectangle lists (full-screen post/UI quads, PS 237) and no tile lighting (VS 1417).
    if ((todos && ancho1280 && !rectangulos && p.vs->numero != 1417) ||
        std::find(numeros.begin(), numeros.end(), ps->numero) != numeros.end()) {
      const deko::Subida id = Reservar(256, DK_UNIFORM_BUF_ALIGNMENT);
      if (id) {
        // Hue from the golden ratio, full saturation: neighbouring numbers get very different colours.
        const float h = std::fmod(float(ps->numero) * 0.61803398875f, 1.0f) * 6.0f;
        const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
        float rgb[3] = {0, 0, 0};
        const int sector = int(h) % 6;
        const float tabla[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
        for (int k = 0; k < 3; ++k) rgb[k] = tabla[sector][k];
        const float v[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
        std::memcpy(id.cpu, v, sizeof(v));
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 5, id.gpu, 256);
        fs = &fs_falso_;
        static uint32_t informes = 0;
        if (informes < 256) {
          static std::vector<uint32_t> vistos;
          if (std::find(vistos.begin(), vistos.end(), ps->numero) == vistos.end()) {
            vistos.push_back(ps->numero);
            ++informes;
            REXLOG_INFO("[colorps] ps {} vs {} -> rgb {:.2f},{:.2f},{:.2f} (pitch {})", ps->numero, p.vs->numero, rgb[0],
                        rgb[1], rgb[2], r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF);
          }
        }
      }
    }
  }
  if (ps && fs_falso_valido_ && REXCVAR_GET(fc3_dk_color_falso) &&
      (r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF) == 880) {
    const deko::Subida id = Reservar(256, DK_UNIFORM_BUF_ALIGNMENT);
    if (id) {
      const float v[4] = {float(p.vs->numero & 0xFF) / 255.0f, float((p.vs->numero >> 8) & 0xFF) / 255.0f,
                          float(ps->numero & 0xFF) / 255.0f, 1.0f};
      std::memcpy(id.cpu, v, sizeof(v));
      dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 5, id.gpu, 256);
      fs = &fs_falso_;
    }
  }
  nucleo_.InvalidarCodigoSiHaceFalta();  // code copied in the middle of a frame (rex/ui/deko3d/nucleo.h)
  if (vs != g_.vs || fs != g_.ps || rectangulos != g_.gs) {
    if (rectangulos) {
      const DkShader* shaders[3] = {vs, &gs_rectangulos_, fs};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 3);
    } else {
      const DkShader* shaders[2] = {vs, fs};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    }
    g_.vs = vs;
    g_.ps = fs;
    g_.gs = rectangulos;
  }

  // --- Fixed state, each block only when it changes
  {
    DkRasterizerState rs = Ceros<DkRasterizerState>();
    dkRasterizerStateDefaults(&rs);
    const bool frente = modo_sc & 0x1, dorso = (modo_sc >> 1) & 0x1;
    rs.cullMode = frente && dorso ? DkFace_FrontAndBack : frente ? DkFace_Front : dorso ? DkFace_Back : DkFace_None;
    // PA_SU_SC_MODE_CNTL.face: 1 = clockwise is the front. deko3d decides the facing in framebuffer coordinates
    // (row 0 at the top) like Vulkan, and the window positions are the same with a flipped viewport.
    rs.frontFace = (modo_sc & 0x4) ? DkFrontFace_CW : DkFrontFace_CCW;
    rs.provokingVertex = DkProvokingVertex_First;  // the Vulkan path's
    rs.depthBiasEnableMask = con_sesgo ? DkPolygonFlag_All : 0;
    if (!g_.valido || !Iguales(rs, g_.raster)) {
      dkCmdBufBindRasterizerState(cmd, &rs);
      g_.raster = rs;
    }
  }
  // For the GPU profile's repeat test: a draw that blends nothing and writes no depth or stencil gives the same
  // image if it is drawn twice.
  bool repetible = true;
  {
    DkColorState cs = Ceros<DkColorState>();
    dkColorStateDefaults(&cs);
    DkColorWriteState cw = Ceros<DkColorWriteState>();
    cw.masks = 0;
    std::array<DkBlendState, 4> mezclas{};
    uint32_t n_colores = 0;
    escala_exp_color_ = 0.0f;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!claves[i]) {
        continue;
      }
      const uint32_t m = r[kMezcla[i]] & 0x1FFF1FFF;
      const uint32_t mascara = (mascaras >> (i * 4)) & 0xF;
      const uint32_t fuente_c = m & 0x1F, op = (m >> 5) & 0x7, destino_c = (m >> 8) & 0x1F;
      const uint32_t fuente_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, destino_a = (m >> 24) & 0x1F;
      // Blending only where it changes a written channel (the Vulkan backend's criterion).
      const bool color_directo = fuente_c == 1 && destino_c == 0 && op == 0;
      const bool alfa_directo = fuente_a == 1 && destino_a == 0 && op_a == 0;
      bool mezcla = ((mascara & 0x7) && !color_directo) || ((mascara & 0x8) && !alfa_directo);
      // fc3-switch (PROTOTYPE): color_exp_bias (RB_COLOR_INFO bits 20-25, signed): the output is scaled by 2^bias
      // before blending (Xenia applies it to the shader's color). Far Cry 3 draws its lighting into a 7e3 target with
      // a bias and resolves it with copy_dest_exp_bias -5; without the write's bias the lit scene came out 0.
      // Done as a ConstColor/ConstAlpha source factor: only when the game's own source factor is ONE and it does not
      // use the blend constant itself.
      const int32_t sesgo_exp = int32_t(r[kInfoColor[i]] << 6) >> 26;
      if (sesgo_exp && REXCVAR_GET(fc3_dk_sesgo_exp_color)) {
        const bool usa_constante = (fuente_c >= 12 && fuente_c <= 15) || (destino_c >= 12 && destino_c <= 15) ||
                                   (fuente_a >= 12 && fuente_a <= 15) || (destino_a >= 12 && destino_a <= 15);
        const float escala = std::ldexp(1.0f, sesgo_exp);
        const bool aplicable = fuente_c == 1 && fuente_a == 1 && !usa_constante &&
                               (escala_exp_color_ == 0.0f || escala_exp_color_ == escala);
        if (sesgos_exp_color_vistos_.insert((uint64_t(r[kInfoColor[i]]) << 32) | (m & 0xFFFF) | (aplicable ? 1u << 31 : 0u))
                .second &&
            sesgos_exp_color_vistos_.size() <= 32) {
          REXLOG_INFO("[deko3d] destino de color con exp bias {} (RB_COLOR_INFO {:08X}, mezcla {:08X}): {}", sesgo_exp,
                      r[kInfoColor[i]], m, aplicable ? "con la constante de mezcla" : "NO aplicado");
        }
        if (aplicable) {
          escala_exp_color_ = escala;
          mezcla = true;
        }
      }
      dkColorStateSetBlendEnable(&cs, i, mezcla);
      dkColorWriteStateSetMask(&cw, i, mascara);
      DkBlendState& b = mezclas[i];
      b = Ceros<DkBlendState>();
      if (mezcla) {
        b.colorBlendOp = OperacionDe(op);
        b.srcColorBlendFactor = FactorDe(fuente_c);
        b.dstColorBlendFactor = FactorDe(destino_c);
        b.alphaBlendOp = OperacionDe(op_a);
        b.srcAlphaBlendFactor = FactorDe(fuente_a);
        b.dstAlphaBlendFactor = FactorDe(destino_a);
        if (escala_exp_color_ != 0.0f && fuente_c == 1 && fuente_a == 1 && int32_t(r[kInfoColor[i]] << 6) >> 26) {
          b.srcColorBlendFactor = DkBlendFactor_ConstColor;
          b.srcAlphaBlendFactor = DkBlendFactor_ConstAlpha;
        }
      }
      n_colores = i + 1;
    }
    repetible = cs.blendEnableMask == 0;
    if (!g_.valido || !Iguales(cs, g_.color)) {
      dkCmdBufBindColorState(cmd, &cs);
      g_.color = cs;
    }
    if (!g_.valido || !Iguales(cw, g_.mascaras)) {
      dkCmdBufBindColorWriteState(cmd, &cw);
      g_.mascaras = cw;
    }
    for (uint32_t i = 0; i < n_colores; ++i) {
      if ((cs.blendEnableMask >> i) & 0x1) {
        if (!g_.valido || !Iguales(mezclas[i], g_.mezclas[i])) {
          dkCmdBufBindBlendState(cmd, i, &mezclas[i]);
          g_.mezclas[i] = mezclas[i];
        }
      }
    }
  }
  {
    DkDepthStencilState ds = Ceros<DkDepthStencilState>();
    ds.depthCompareOp = DkCompareOp_Always;
    ds.stencilFrontFailOp = ds.stencilFrontPassOp = ds.stencilFrontDepthFailOp = DkStencilOp_Keep;
    ds.stencilBackFailOp = ds.stencilBackPassOp = ds.stencilBackDepthFailOp = DkStencilOp_Keep;
    ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;
    if (claves[4]) {
      const uint32_t d = control_profundidad;
      ds.depthTestEnable = (d >> 1) & 0x1;
      ds.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      ds.depthCompareOp = DkCompareOp(((d >> 4) & 0x7) + 1);  // the Xenos functions in deko3d's order, from 1
      ds.stencilTestEnable = d & 0x1;
      if (ds.stencilTestEnable && p.ps) {
        static std::vector<uint32_t> sin_estencil;
        static bool leido = false;
        if (!leido) {
          leido = true;
          const std::string lista = REXCVAR_GET(fc3_dk_sin_estencil_ps);
          size_t i = 0;
          while (i < lista.size()) {
            const size_t j = lista.find(',', i);
            const std::string t = lista.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!t.empty()) sin_estencil.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 10)));
            if (j == std::string::npos) break;
            i = j + 1;
          }
          if (!sin_estencil.empty()) REXLOG_WARN("[deko3d] fc3_dk_sin_estencil_ps: {} PS without stencil test (HACK)", sin_estencil.size());
        }
        if (!sin_estencil.empty() &&
            std::find(sin_estencil.begin(), sin_estencil.end(), p.ps->numero) != sin_estencil.end()) {
          ds.stencilTestEnable = false;
        }
      }
      repetible = repetible && !ds.depthWriteEnable && !ds.stencilTestEnable;
      ds.stencilFrontFailOp = DkStencilOp(((d >> 11) & 0x7) + 1);
      ds.stencilFrontPassOp = DkStencilOp(((d >> 14) & 0x7) + 1);
      ds.stencilFrontDepthFailOp = DkStencilOp(((d >> 17) & 0x7) + 1);
      ds.stencilFrontCompareOp = DkCompareOp(((d >> 8) & 0x7) + 1);
      if ((d >> 7) & 0x1) {
        ds.stencilBackCompareOp = DkCompareOp(((d >> 20) & 0x7) + 1);
        ds.stencilBackFailOp = DkStencilOp(((d >> 23) & 0x7) + 1);
        ds.stencilBackPassOp = DkStencilOp(((d >> 26) & 0x7) + 1);
        ds.stencilBackDepthFailOp = DkStencilOp(((d >> 29) & 0x7) + 1);
      } else {
        ds.stencilBackCompareOp = ds.stencilFrontCompareOp;
        ds.stencilBackFailOp = ds.stencilFrontFailOp;
        ds.stencilBackPassOp = ds.stencilFrontPassOp;
        ds.stencilBackDepthFailOp = ds.stencilFrontDepthFailOp;
      }
      const uint32_t frente = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
      const uint32_t dorso = ((d >> 7) & 0x1) ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF] : frente;
      if (claves[0] && ((claves[0] >> 16) & 0xFFFFF) == 3 && ((claves[4] >> 24) & 0xFFF) == 0x540 &&
          ds.stencilTestEnable && ds.stencilFrontCompareOp == DkCompareOp_Equal && (frente & 255) == 1) {
        static const int mode = REXCVAR_GET(fc3_light_test);
        if (mode & 1) ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;
        if (mode & 2) ds.depthCompareOp = DkCompareOp_Always;
        static bool reported = false;
        if (!reported) {
          REXLOG_INFO("[fc3-light-test] mode={} depthcontrol={:08X} stencil={:08X}", mode, d, frente);
          reported = true;
        }
      }
      if (ds.stencilTestEnable && (!g_.valido || frente != g_.stencil[0] || dorso != g_.stencil[1])) {
        // ref in bits 0-7, compare mask in 8-15, write mask in 16-23
        dkCmdBufSetStencil(cmd, DkFace_Front, uint8_t(frente >> 16), uint8_t(frente), uint8_t(frente >> 8));
        dkCmdBufSetStencil(cmd, DkFace_Back, uint8_t(dorso >> 16), uint8_t(dorso), uint8_t(dorso >> 8));
        g_.stencil[0] = frente;
        g_.stencil[1] = dorso;
      }
    }
    if (!g_.valido || !Iguales(ds, g_.profundidad)) {
      dkCmdBufBindDepthStencilState(cmd, &ds);
      g_.profundidad = ds;
    }
  }
  {
    float mezcla_constante[4] = {Flotante(r[gr::XE_GPU_REG_RB_BLEND_RED]), Flotante(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                 Flotante(r[gr::XE_GPU_REG_RB_BLEND_BLUE]), Flotante(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    if (escala_exp_color_ != 0.0f) {  // fc3-switch: color_exp_bias (above)
      std::fill(std::begin(mezcla_constante), std::end(mezcla_constante), escala_exp_color_);
    }
    if (!g_.valido || std::memcmp(mezcla_constante, g_.mezcla_constante, sizeof(mezcla_constante)) != 0) {
      dkCmdBufSetBlendConst(cmd, mezcla_constante[0], mezcla_constante[1], mezcla_constante[2], mezcla_constante[3]);
      std::memcpy(g_.mezcla_constante, mezcla_constante, sizeof(mezcla_constante));
    }
    // No clamp, as Mesa programs it (HowToDeko3d.md, section 6, item 8); deko3d doubles the units like nvc0.
    if (con_sesgo && (!g_.valido || std::memcmp(sesgo, g_.sesgo, sizeof(sesgo)) != 0)) {
      dkCmdBufSetDepthBias(cmd, sesgo[0], 0.0f, sesgo[1]);
      std::memcpy(g_.sesgo, sesgo, sizeof(sesgo));
    }
    if (!g_.valido || reinicio != g_.reinicio) {
      dkCmdBufSetPrimitiveRestart(cmd, reinicio, 0xFFFFFFFFu);
      g_.reinicio = reinicio;
    }
  }
  {
    // A negative height (a flipped target) is a positive one with the clip-space y negated: deko3d computes the
    // viewport rectangle unsigned (HowToDeko3d.md, section 6, item 2).
    const bool volteado = viewport[3] < 0.0f;
    float vp[6];
    std::memcpy(vp, viewport, sizeof(vp));
    if (volteado) {
      vp[1] = viewport[1] + viewport[3];
      vp[3] = -viewport[3];
    }
    if (!g_.valido || std::memcmp(vp, g_.viewport, sizeof(vp)) != 0) {
      const DkViewport v = {vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]};
      dkCmdBufSetViewports(cmd, 0, &v, 1);
      std::memcpy(g_.viewport, vp, sizeof(vp));
    }
    if (!g_.valido || volteado != g_.volteado) {
      const DkViewportSwizzle sw = {DkSwizzle_PositiveX, volteado ? DkSwizzle_NegativeY : DkSwizzle_PositiveY,
                                    DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
      dkCmdBufSetViewportSwizzles(cmd, 0, &sw, 1);
      g_.volteado = volteado;
    }
    if (!g_.valido || !Iguales(tijera, g_.tijera)) {
      dkCmdBufSetScissors(cmd, 0, &tijera, 1);
      g_.tijera = tijera;
    }
  }
  if (!g_.valido || entrada != g_.entrada) {
    dkCmdBufBindVtxAttribState(cmd, entrada->atributos.data(), entrada->n_atributos);
    dkCmdBufBindVtxBufferState(cmd, entrada->estado_bufers.data(), uint32_t(entrada->enlaces.size()));
    g_.entrada = entrada;
  }
  if (n_texturas && (!g_.valido || n_texturas > g_.n_texturas ||
                     std::memcmp(texturas.data(), g_.texturas.data(), n_texturas * sizeof(DkResHandle)) != 0)) {
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, texturas.data(), n_texturas);
    std::copy(texturas.begin(), texturas.begin() + n_texturas, g_.texturas.begin());
    g_.n_texturas = std::max(g_.n_texturas, n_texturas);
  }
  g_.valido = true;

  // --- Uploads: vertices, indices and constants (the space was checked above: this slot)
  cortar_etapa(2);
  std::array<DkBufExtents, 16> bufers{};
  for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
    const Origen& origen = origenes[b];
    // fc3-switch 2026-11-08 DIAGNOSTIC (triangle hunt): VS 1051's per-object record (the stream at slot 93) on every
    // draw of a frame every 1000 frames: guest address, bytes and the first 32 bytes as the guest wrote them.
    if (p.vs->numero == 1051 && entrada->enlaces[b].ranura == 93 && (fotograma_ % 1000) == 0) {
      const uint8_t* g = origen.datos;
      std::string bytes;
      for (uint32_t k = 0; k < std::min<uint32_t>(origen.bytes, 32); ++k) bytes += fmt::format("{:02X}", g[k]);
      REXLOG_INFO("[vs1051] frame {} draw {} slot 93 addr {:08X} bytes {} stride {} first32 {}", fotograma_,
                  vigia_dibujos_.load(std::memory_order_relaxed),
                  uint32_t(r[kRegFetch + 93 * 2] & 0x1FFFFFFC), origen.bytes, entrada->enlaces[b].zancada, bytes);
    }
    const bool sin_cache = REXCVAR_GET(fc3_dk_vertices_sin_cache);
    // Kept from an earlier frame with the same content (or kept now, the second time it is seen unchanged).
    if (geometria_.activo() && !sin_cache) {
      const deko::AlmacenGeometria::Resultado en_almacen = geometria_.Buscar(
          origen.datos, origen.bytes, uint64_t(origen.orden), origen.bytes, [&](uint8_t* destino, uint32_t&, uint32_t&) {
            CopiarVertices(TrabajoCopia{origen.datos, destino, origen.bytes / 4, origen.orden});
          });
      if (en_almacen.gpu) {
        geometria_.Usar(en_almacen.entrada, trabajo_);
        bufers[b] = {en_almacen.gpu, origen.bytes};
        continue;
      }
    }
    // The same guest range in the same order already copied in this work slot: bound again, not copied.
    const uint64_t direccion = uint64_t(reinterpret_cast<uintptr_t>(origen.datos));
    uint64_t ya = 0;
    if (!sin_cache && dedupe_.Buscar(direccion, origen.bytes, uint32_t(origen.orden), ya)) {
      bufers[b] = {DkGpuAddr(ya), origen.bytes};
      continue;
    }
    const deko::Subida s = nucleo_.Reservar(origen.bytes, 16);
    if (!s) {
      return Rechazar(13, "sin sitio en el anillo de subida");
    }
    CopiarVertices(TrabajoCopia{origen.datos, s.cpu, origen.bytes / 4, origen.orden});
    dedupe_.Anotar(direccion, origen.bytes, uint32_t(origen.orden), uint64_t(s.gpu));
    bufers[b] = {s.gpu, origen.bytes};
  }
  // One call per buffer. deko3d 0.5.0's dkCmdBufBindVtxBuffers reserves 5 words per buffer and writes 6: with n
  // buffers near the end of a command memory piece, up to n - 1 words land in the next piece, the next command
  // overwrites them and the GPU reads that command's header as data (the console's GPU method error of 2026-10-09,
  // HowToDeko3d.md, section 6, item 17). With one buffer, the 5 reserved words leave room for the sixth.
  for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
    dkCmdBufBindVtxBuffers(cmd, uint32_t(b), &bufers[b], 1);
  }
  // The fetches the shader does itself (EntradaVerticesDk::manuales): each buffer whole, as a storage buffer (its
  // words in host order, as the vertex buffers), and the descriptors in uniform buffer 3 of the vertex stage.
  if (!entrada->manuales.empty()) {
    for (size_t b = 0; b < entrada->ranuras_manuales.size(); ++b) {
      const uint32_t d0 = r[kRegFetch + entrada->ranuras_manuales[b] * 2];
      const uint32_t d1 = r[kRegFetch + entrada->ranuras_manuales[b] * 2 + 1];
      const uint64_t direccion = uint64_t(d0 & 0x1FFFFFFC);
      const uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex) || !bytes || bytes > (8u << 20) ||
          direccion + bytes > kMemoriaFisica) {
        return Rechazar(27, "bufer de vertices leido por el shader invalido o demasiado grande");
      }
      const deko::Subida s = nucleo_.Reservar(bytes, 16);
      if (!s) {
        return Rechazar(13, "sin sitio en el anillo de subida");
      }
      CopiarVertices(TrabajoCopia{memoria_->TranslatePhysical(uint32_t(direccion)), s.cpu, bytes / 4,
                                  static_cast<xenos::Endian>(d1 & 0x3)});
      dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, uint32_t(b), s.gpu, bytes);
    }
    const deko::Subida descriptores = nucleo_.Reservar(512, DK_UNIFORM_BUF_ALIGNMENT);
    if (!descriptores) {
      return Rechazar(13, "sin sitio en el anillo de subida");
    }
    std::array<uint32_t, 128> v{};
    for (const EntradaVerticesDk::Manual& m : entrada->manuales) {
      if (m.k >= 32) {
        continue;
      }
      uint32_t b = 0;
      while (entrada->ranuras_manuales[b] != m.ranura) {
        ++b;
      }
      v[m.k * 4 + 0] = b | (m.formato << 8) | (uint32_t(m.con_signo) << 16) | (uint32_t(m.entero) << 17);
      v[m.k * 4 + 1] = m.offset;
      v[m.k * 4 + 2] = m.zancada;
      v[m.k * 4 + 3] = m.remapeo;
      // fc3-switch 2026-11-08 DIAGNOSTIC: VS 1051's own fetches (descriptor k, fetch slot, endian, format, offset,
      // stride, remap code) every 1000 frames.
      if ((p.vs->numero == 1051 || p.vs->numero == 1632) && (fotograma_ % 1000) == 0) {
        REXLOG_INFO("[vs1051m] vs {} frame {} k {} slot {} endian {} fmt {} off {} stride {} remap {:03X} signed {} int {} c16 {}",
                    p.vs->numero, fotograma_, m.k, m.ranura, r[kRegFetch + m.ranura * 2 + 1] & 0x3, m.formato, m.offset, m.zancada,
                    m.remapeo, m.con_signo, m.entero, m.componentes16);
      }
      if (m.componentes16 && (REXCVAR_GET(fc3_dk_mitades_8in32) || MitadesManualActivas(fotograma_)) &&
          static_cast<xenos::Endian>(r[kRegFetch + m.ranura * 2 + 1] & 0x3) == xenos::Endian::k8in32) {
        v[m.k * 4 + 3] = RemapeoMitades(m.remapeo, m.componentes16);
      }
    }
    std::memcpy(descriptores.cpu, v.data(), sizeof(v));
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 3, descriptores.gpu, 512);
  }
  cortar_etapa(3);
  DkGpuAddr dir_indices = 0;
  if (con_indices) {
    if (indices_almacen.gpu) {
      geometria_.Usar(indices_almacen.entrada, trabajo_);
      dir_indices = indices_almacen.gpu;
    } else {
      const deko::Subida s = nucleo_.Reservar(bytes_indices, 16);
      if (!s) {
        return Rechazar(13, "sin sitio en el anillo de subida");
      }
      uint32_t minimo, maximo;
      if (indices_de_16) {
        IndicesDe16<true>(indices_datos, n_indices, reinterpret_cast<uint16_t*>(s.cpu),
                          indices_orden == xenos::Endian::k8in16, minimo, maximo);
      } else {
        IndicesDe32<true>(indices_datos, n_indices, indices32, indices_orden, reinicio, indice_reinicio, desplazamiento,
                          reinterpret_cast<uint32_t*>(s.cpu), minimo, maximo);
      }
      dir_indices = s.gpu;
    }
    dkCmdBufBindIdxBuffer(cmd, indices_de_16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, dir_indices);
  }
  cortar_etapa(4);
  const bool por_diferencias = REXCVAR_GET(nfsmw_dk_constantes_por_diferencias);  // read each time: the game can switch it (ds2 safe mode)
  if (por_diferencias && !es_cielo) {
    constexpr uint32_t kBloqueCompartidas = 512;  // dkCmdBufPushConstants wants the block size in 256-byte steps
    if (bloques_trabajo_ != trabajo_) {
      // A new work: the blocks start as the registers are now (the GPU has not started this work: CPU writes).
      const deko::Subida v = nucleo_.Reservar(kUboBytesVs, DK_UNIFORM_BUF_ALIGNMENT);
      const deko::Subida f = nucleo_.Reservar(kUboBytesPs, DK_UNIFORM_BUF_ALIGNMENT);
      const deko::Subida c = nucleo_.Reservar(kBloqueCompartidas, DK_UNIFORM_BUF_ALIGNMENT);
      if (!v || !f || !c) {
        return Rechazar(13, "sin sitio en el anillo de subida");
      }
      std::memcpy(v.cpu, r + kRegConstantesVs, kUboBytesVs);
      std::memcpy(sombra_vs_.data(), r + kRegConstantesVs, kUboBytesVs);
      std::memcpy(f.cpu, r + kRegConstantesPs, kUboBytesPs);
      std::memcpy(sombra_ps_.data(), r + kRegConstantesPs, kUboBytesPs);
      std::memcpy(c.cpu, compartidas, sizeof(compartidas));
      std::memcpy(sombra_compartidas_.data(), compartidas, sizeof(compartidas));
      bloque_vs_ = v.gpu;
      bloque_ps_ = f.gpu;
      bloque_compartidas_ = c.gpu;
      bloques_trabajo_ = trabajo_;
      bloque_vs_generacion_ = p.generacion_constantes_vs;
      bloque_ps_generacion_ = p.generacion_constantes_ps;
      bloque_vs_cubierto_ = kUboBytesVs;
      bloque_ps_cubierto_ = kUboBytesPs;
    } else {
      // What changed since what was pushed, within what this draw's shaders read.
      if (bloque_vs_generacion_ != p.generacion_constantes_vs) {
        bloque_vs_generacion_ = p.generacion_constantes_vs;
        bloque_vs_cubierto_ = 0;
      }
      if (bytes_vs > bloque_vs_cubierto_) {
        registros_empujados_ += EmpujarDiferencias(cmd, bloque_vs_, kUboBytesVs, r + kRegConstantesVs,
                                                   sombra_vs_.data(), bloque_vs_cubierto_, bytes_vs, empujes_);
        bloque_vs_cubierto_ = bytes_vs;
      }
      if (ps) {
        if (bloque_ps_generacion_ != p.generacion_constantes_ps) {
          bloque_ps_generacion_ = p.generacion_constantes_ps;
          bloque_ps_cubierto_ = 0;
        }
        if (bytes_ps > bloque_ps_cubierto_) {
          registros_empujados_ += EmpujarDiferencias(cmd, bloque_ps_, kUboBytesPs, r + kRegConstantesPs,
                                                     sombra_ps_.data(), bloque_ps_cubierto_, bytes_ps, empujes_);
          bloque_ps_cubierto_ = bytes_ps;
        }
      }
      registros_empujados_ += EmpujarDiferencias(cmd, bloque_compartidas_, kBloqueCompartidas, compartidas,
                                                 sombra_compartidas_.data(), 0, sizeof(compartidas), empujes_);
    }
    if (bloque_vs_ != g_.ubo[0]) {
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, bloque_vs_, kUboBytesVs);
      g_.ubo[0] = bloque_vs_;
    }
    if (ps && bloque_ps_ != g_.ubo[1]) {
      dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, bloque_ps_, kUboBytesPs);
      g_.ubo[1] = bloque_ps_;
    }
    if (bloque_compartidas_ != g_.ubo[2]) {
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, bloque_compartidas_, kBloqueCompartidas);
      dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, bloque_compartidas_, kBloqueCompartidas);
      g_.ubo[2] = bloque_compartidas_;
    }
  } else {
  // Only the registers each shader reads (EntradaShader::constantes_bytes); the copy is reused while their
  // generation and the slot are the same and it covers what this shader needs. The deferred sky always gets its own
  // copies (they stay as they are until it is replayed).
  if (constantes_trabajo_ != trabajo_ || es_cielo) {
    constantes_trabajo_ = trabajo_;
    constantes_vs_generacion_ = constantes_ps_generacion_ = UINT64_MAX;
  }
  if (constantes_vs_generacion_ != p.generacion_constantes_vs || bytes_vs > constantes_vs_bytes_) {
    const deko::Subida s = nucleo_.Reservar(std::max(bytes_vs, kUboBytesVs), DK_UNIFORM_BUF_ALIGNMENT);
    if (!s) {
      return Rechazar(13, "sin sitio en el anillo de subida");
    }
    std::memcpy(s.cpu, r + kRegConstantesVs, bytes_vs);
    constantes_vs_gpu_ = s.gpu;
    constantes_vs_generacion_ = p.generacion_constantes_vs;
    constantes_vs_bytes_ = bytes_vs;
  }
  if (ps && (constantes_ps_generacion_ != p.generacion_constantes_ps || bytes_ps > constantes_ps_bytes_)) {
    const deko::Subida s = nucleo_.Reservar(std::max(bytes_ps, kUboBytesPs), DK_UNIFORM_BUF_ALIGNMENT);
    if (!s) {
      return Rechazar(13, "sin sitio en el anillo de subida");
    }
    std::memcpy(s.cpu, r + kRegConstantesPs, bytes_ps);
    constantes_ps_gpu_ = s.gpu;
    constantes_ps_generacion_ = p.generacion_constantes_ps;
    constantes_ps_bytes_ = bytes_ps;
  }
  if (compartidas_trabajo_ != trabajo_ || es_cielo ||
      std::memcmp(compartidas, compartidas_previas_, sizeof(compartidas)) != 0) {
    const deko::Subida s = nucleo_.Reservar(kUboBytesCompartidas, DK_UNIFORM_BUF_ALIGNMENT);
    if (!s) {
      return Rechazar(13, "sin sitio en el anillo de subida");
    }
    std::memcpy(s.cpu, compartidas, sizeof(compartidas));
    std::memcpy(compartidas_previas_, compartidas, sizeof(compartidas));
    compartidas_gpu_ = s.gpu;
    compartidas_trabajo_ = trabajo_;
  }
  if (constantes_vs_gpu_ != g_.ubo[0]) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, constantes_vs_gpu_, kUboBytesVs);
    g_.ubo[0] = constantes_vs_gpu_;
  }
  if (ps && constantes_ps_gpu_ != g_.ubo[1]) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, constantes_ps_gpu_, kUboBytesPs);
    g_.ubo[1] = constantes_ps_gpu_;
  }
  if (compartidas_gpu_ != g_.ubo[2]) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, compartidas_gpu_, kUboBytesCompartidas);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, compartidas_gpu_, kUboBytesCompartidas);
    g_.ubo[2] = compartidas_gpu_;
  }
  }

  // --- The draw
  if (es_cielo) {
    // Deferred: its state is what g_ holds now; its buffers stay in this slot's ring until the work is submitted, and
    // the work is not closed with it pending (CerrarTrabajo emits it).
    cielo_.estado = g_;
    cielo_.bufers = bufers;
    cielo_.n_bufers = uint32_t(entrada->enlaces.size());
    cielo_.con_indices = con_indices;
    cielo_.indices_de_16 = indices_de_16;
    cielo_.indices = dir_indices;
    cielo_.primitiva = primitiva;
    cielo_.n_indices = n_indices;
    cielo_.vmin = vmin;
    cielo_.cuenta = cuenta;
    cielo_pendiente_ = true;
    ++cielo_aplazados_;
  } else {
    // A profiled frame (nfsmw_dk_perfil_gpu_s): a timestamp before and after the draw, 32 bytes per draw.
    const bool marcar = perfilando_ && perfil_marcas_.size() < kMarcasPerfil;
    // The repeat test: the first draw of a pass, when drawing it again changes nothing, is drawn and timed twice. A
    // second time much faster than the first means the first paid for something else (a flush, a wait), not for
    // its pixels.
    const bool repetir = marcar && repetible && dibujos_en_pase_ == 0 && perfil_marcas_.size() + 5 < kMarcasPerfil;
    // nfsmw_dk_perfil_variantes: a full-screen textured draw of that kind is also drawn, before its real second copy
    // (the last one decides the image), with sampler 0 (bilinear, clamped, base level) on every texture (2), with the
    // empty 1x1 textures (3), and writing only target 0 (4): which part of its cost is the sampling, the texture
    // reads or the outputs.
    const uint32_t elementos = con_indices ? n_indices : cuenta;
    const bool variantes = repetir && n_texturas && elementos <= 6 && REXCVAR_GET(nfsmw_dk_perfil_variantes);
    uint32_t veces[5] = {0, 1};
    uint32_t n_veces = repetir ? 2 : 1;
    if (variantes) {
      n_veces = 0;
      veces[n_veces++] = 0;
      veces[n_veces++] = 2;
      veces[n_veces++] = 3;
      if (mascaras & 0xFFF0) {
        veces[n_veces++] = 4;
      }
      veces[n_veces++] = 1;
    }
    // nfsmw_dk_perfil_barrera: the first draw of each pass (and every copy of the tests) is timed after the GPU has
    // finished everything before it, so no earlier work (a copy, a clear, a wait) is counted as the draw's.
    const bool barrera = marcar && (dibujos_en_pase_ == 0) && REXCVAR_GET(nfsmw_dk_perfil_barrera);
    // fc3-switch 2026-11-11 DIAGNOSTIC (fc3_dk_barrera_ps, AO specks test BA1): full barriers around the listed PS.
    bool barrera_ps = false;
    {
      static std::vector<uint32_t> lista_ps;
      static std::vector<uint32_t> avisados;
      static bool leido = false;
      if (!leido) {
        leido = true;
        const std::string lista = REXCVAR_GET(fc3_dk_barrera_ps);
        size_t i = 0;
        while (i < lista.size()) {
          const size_t j = lista.find(',', i);
          const std::string t = lista.substr(i, j == std::string::npos ? std::string::npos : j - i);
          if (!t.empty()) lista_ps.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 10)));
          if (j == std::string::npos) break;
          i = j + 1;
        }
        if (!lista_ps.empty()) REXLOG_WARN("[deko3d] fc3_dk_barrera_ps: {} PS fenced (DIAGNOSTIC)", lista_ps.size());
      }
      if (!lista_ps.empty() && p.ps && std::find(lista_ps.begin(), lista_ps.end(), p.ps->numero) != lista_ps.end()) {
        barrera_ps = true;
        if (std::find(avisados.begin(), avisados.end(), p.ps->numero) == avisados.end()) {
          avisados.push_back(p.ps->numero);
          REXLOG_WARN("[deko3d] fc3_dk_barrera_ps: barriers around PS {} (first at frame {}, DIAGNOSTIC)",
                      p.ps->numero, fotograma_);
        }
      }
    }
    for (uint32_t k = 0; k < n_veces; ++k) {
      const uint32_t vez = veces[k];
      const DkGpuAddr marca_vez = perfil_gpu_ + DkGpuAddr(perfil_marcas_.size()) * 32;
      if (vez == 2 || vez == 3) {
        std::array<DkResHandle, 32> otras = texturas;
        for (uint32_t t = 0; t < n_texturas; ++t) {
          otras[t] = vez == 2 ? (otras[t] & 0xFFFFF) : dkMakeTextureHandle(0, 0);
        }
        if (vez == 3 && ps) {
          for (const SamplerShader& sampler : ps->samplers) {
            if (sampler.registro < 16) {
              otras[sampler.registro] = dkMakeTextureHandle(VaciaDe(sampler.tipo), 0);
            }
          }
        }
        dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, otras.data(), n_texturas);
      } else if (vez == 4) {
        DkColorWriteState solo0 = g_.mascaras;
        dkColorWriteStateSetMask(&solo0, 1, 0);
        dkColorWriteStateSetMask(&solo0, 2, 0);
        dkColorWriteStateSetMask(&solo0, 3, 0);
        dkCmdBufBindColorWriteState(cmd, &solo0);
      }
      if (marcar) {
        if (barrera) {
          dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
        }
        dkCmdBufReportCounter(cmd, DkCounter_Timestamp, marca_vez);
      }
      if (primitiva == DkPrimitive_Points) {
        // PA_SU_POINT_SIZE: half height (bits 0-15) and half width (16-31) in 12.4 fixed point.
        const uint32_t tam = r[gr::XE_GPU_REG_PA_SU_POINT_SIZE];
        const float lado = float(std::max(tam & 0xFFFF, tam >> 16)) * (2.0f / 16.0f);
        dkCmdBufSetPointSize(cmd, lado > 0.0f ? lado : 1.0f);
      }
      if (barrera_ps) {  // DIAGNOSTIC fc3_dk_barrera_ps
        dkCmdBufBarrier(cmd, DkBarrier_Full,
                        DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                            DkInvalidateFlags_L2Cache);
      }
      if (con_indices) {
        dkCmdBufDrawIndexed(cmd, primitiva, n_indices, 1, 0, -int32_t(vmin), 0);
      } else {
        dkCmdBufDraw(cmd, primitiva, cuenta, 1, 0, 0);
      }
      if (barrera_ps) {  // DIAGNOSTIC fc3_dk_barrera_ps
        dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
      }
      if (volcando_ && k + 1 == n_veces && REXCVAR_GET(fc3_sonda)) {  // DIAGNOSTIC fc3_sonda: probes after this draw
        dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
        const uint32_t paso = sonda_paso_++;
        if (imagenes_pase_[0]) Sondear(*imagenes_pase_[0], paso, ps ? ps->numero : 0xFFFF, p.vs->numero, claves[0], 0);
        if (imagenes_pase_[4]) Sondear(*imagenes_pase_[4], paso, ps ? ps->numero : 0xFFFF, p.vs->numero, claves[4], 1);
      }
      if (marcar) {
        dkCmdBufReportCounter(cmd, DkCounter_Timestamp, marca_vez + 16);
        perfil_marcas_.push_back({vs, fs, g_.pase_serie, categoria_pase_, g_.pase_ancho, g_.pase_alto, claves[0],
                                  claves[4], elementos, std::abs(viewport[2]), std::abs(viewport[3]), tijera.width,
                                  tijera.height, mascaras, control_profundidad, tex0_ancho, tex0_alto, tex0_formato,
                                  tex0_sampler, n_texturas, vez});
        perfil_trabajo_ = trabajo_;
      }
      if (vez == 2 || vez == 3) {
        dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, texturas.data(), n_texturas);
      } else if (vez == 4) {
        dkCmdBufBindColorWriteState(cmd, &g_.mascaras);
      }
    }
    // The first variant draw of a shader: its pixel constants 0-16 once (the downsample's tap offsets).
    if (variantes && ps && constantes_registradas_.insert(fs).second) {
      std::string c;
      for (uint32_t i = 0; i < 17; ++i) {
        float v[4];
        std::memcpy(v, r + kRegConstantesPs + i * 4, sizeof(v));
        c += fmt::format(" c{}=({:.5g},{:.5g},{:.5g},{:.5g})", i, v[0], v[1], v[2], v[3]);
      }
      REXLOG_INFO("[deko3d] perfil GPU: constantes del PS n{} en su primera prueba de variantes:{}", ps->numero, c);
    }
    ++dibujos_en_pase_;
  }
  ++dibujados_;
  cortar_etapa(5);
  ns_grabando_ += deko::AhoraNs() - inicio_ns - (nucleo_.NsEsperandoValla() - espera_inicio_ns);
  return true;
}

}  // namespace rex::x360
