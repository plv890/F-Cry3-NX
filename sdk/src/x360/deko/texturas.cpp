// nfsmw - deko3d renderer: the game's textures, samplers and image descriptors. See nfsmw_dk_render.h.
//
// The texture cache is the Vulkan backend's (nfsmw_nativo_dibujos.cpp, PrepararTextura), with the same keys, mip
// rules, untiling and check intervals: a texture's guest bytes are fingerprinted, and only untiled and uploaded
// when the fingerprint changes; textures that do not change are checked less and less often, down to once every
// 32-39 frames. What deko3d removes: an image is a layout over the image heap (no allocation, no bind thread), and
// the upload is a copy recorded in the slot's pre-list.
//
// Not yet: the sampled re-check of stable textures, the per-frame check budget and the fingerprint thread (P4), and
// cubemaps made of resolved faces (P3).

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <xxhash.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <cmath>
#include <cstring>

#include <rex/x360/deko/render.h>
#include <rex/x360/deko/texture_bias.h>
#include <rex/x360/xenos.h>

// fc3-switch 2026-11-09 DIAGNOSTIC: comma-separated hex base addresses whose guest bytes (base level and mips) are
// written once each to sdmc:/switch/fc3-nx/texcruda_<base>_{base,mips}.bin, to decode them on the PC.
// fc3-switch 2026-11-12 EXPERIMENT (default off): pass the biased-channel mask (TextureSign 2 = 2x - 1) of every
// regular 2D texture without a signed host view to the shaders, not only of BC1. Needs a library whose PS apply
// fc3Bias to their fetches (fc3_biased_texture_experiment must be on too).
REXCVAR_DEFINE_BOOL(fc3_sesgo_todos_formatos, false, "FC3",
                    "EXPERIMENT: biased-channel mask for every 2D texture format, not only BC1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(fc3_texcruda_bases, "", "FC3", "DIAGNOSTIC: hex texture bases whose guest bytes are dumped once");
// fc3-switch 2026-11-09: resolved 8_8_8_8 textures fetched with exp_adjust != 0 are read through a copy scaled by
// 2^exp_adjust (FC3's FXAA 360 taps; off = as before).
REXCVAR_DEFINE_BOOL(fc3_dk_exp_adjust_float16, false, "FC3",
                    "Apply exp_adjust to resolved 16_16_16_16_FLOAT textures (half-res HDR read with -14)");
REXCVAR_DEFINE_BOOL(fc3_dk_exp_adjust_rgba8, false, "FC3", "Apply exp_adjust to resolved 8_8_8_8 textures (FXAA 360)");
REXCVAR_DEFINE_BOOL(fc3_trace_texture_reuploads, false, "FC3",
                    "Measure whether changed guest texture ranges produce identical uploaded pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fc3_skip_identical_texture_uploads, false, "FC3",
                    "Skip texture uploads when all decoded bytes match the previous upload")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_dk_16_16_fijo, false, "NFSMW",
                    "deko3d: un destino k_16_16 (coma fija, -32..32) leido como textura k_16_16 da los bits como entero "
                    "normalizado (o el valor / 32 con signo), por 2^exp_adjust de la constante de la textura, en vez del "
                    "valor tal cual. Apagado: Gears of War 2 y Dead Space 3 leen el valor escrito (Dead Space 3 lo "
                    "escala al resolver, copy_dest_exp_bias -5, que la copia aplica)");
// fc3-switch 2026-11-09 (EXPERIMENT, default off): a texture whose base level is already in the packed mip tail
// (short side <= 16 texels, packed_mips set) is read at the tail's origin instead of at its level-0 offset inside the
// tail. Candidate for Jason's black arm (PS 467 multiplies the albedo by a 4x4 DXT1 texture).
REXCVAR_DEFINE_BOOL(fc3_dk_pequenas_blancas, false, "fc3 enhancements",
                    "deko3d DIAGNOSTIC: textures of 4x4 texels or less are replaced by a white texture (black arm A/B)");
REXCVAR_DEFINE_BOOL(fc3_dk_cola_base_en_origen, false, "fc3 enhancements",
                    "deko3d EXPERIMENT: packed-tail base level read at the tail origin (no level-0 offset)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_mipmaps, true, "NFSMW",
                    "Renderizador nativo: sube los niveles de mip que trae el juego (como en la Xbox 360). false: solo "
                    "el nivel base, como antes de la build 136")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_anisotropico, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 179): filtrado anisotropico de las texturas del mundo "
                     "(samplers lineales con mips). 0 = como la Xbox 360 (trilineal, borroso de lado y a lo lejos); "
                     "2, 4, 8 o 16 = mas nitido de lado, algo mas de GPU")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_mb_max, 512, "NFSMW",
                     "Renderizador nativo: MB de la cache de texturas antes de soltar las que no se usan desde hace "
                     "120 fotogramas (hasta quedar en el 75 %)")
    .range(64, 2048);
// The Vulkan path's texture checks, same names and defaults (nfsmw_nativo_dibujos.cpp is not in the deko3d build).
// fc3-switch 2026-11-12 EXPERIMENT (default off): 3D textures re-read and re-hashed every frame (no interval doubling).
// FC3's volumetric ambient probes (PS 760) are toroidal REPEAT volumes the game rewrites slice by slice as the camera
// moves; a check every up to 32 frames could leave stale slices.
REXCVAR_DEFINE_BOOL(fc3_volumen_cada_fotograma, false, "FC3", "EXPERIMENT: re-check 3D textures every frame");
REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_muestreo, 8, "NFSMW",
                     "Renderizador nativo: las texturas ESTABLES se recomprueban con la huella de una muestra (primer y "
                     "ultimo bloque de 4 KB y 1 de cada 8, base y mips) en vez de con todos sus bytes; si la muestra "
                     "cambia se sigue por el camino completo. N = 1 de cada N recomprobaciones de cada textura sigue "
                     "siendo completa, y las 3000 primeras hacen las dos huellas: con un solo desacuerdo se apaga solo. "
                     "0 = siempre la huella completa")
    .range(0, 64);
REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_kb_fotograma, 6144, "NFSMW",
                     "Renderizador nativo: KB de memoria del invitado que las comprobaciones de texturas estables "
                     "pueden leer en un fotograma; las que no caben esperan 1-2 fotogramas (8 veces seguidas como "
                     "mucho). Reparte las comprobaciones que coincidian en el mismo fotograma. 0 = sin limite");

namespace rex::x360 {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);
constexpr uint64_t kFotogramasSinUsoParaSoltar = 120;
constexpr uint32_t kDescriptoresReservados = 8;  // 0-2 empty textures, the rest free for the renderer

// The sampler's type in the constant table (D3DXPARAMETER_TYPE): what the translator declared the texture as.
bool TipoVolumen(uint16_t tipo) { return tipo == 8 || tipo == 13; }  // Texture3D, Sampler3D
bool TipoCubo(uint16_t tipo) { return tipo == 9 || tipo == 14; }     // TextureCube, SamplerCube

uint32_t Log2Bytes(uint32_t bytes) { return bytes >= 16 ? 4 : bytes >= 8 ? 3 : bytes >= 4 ? 2 : bytes >= 2 ? 1 : 0; }

// The fetch constant's swizzle through the channels the host format has (GuestToHostSwizzle; MapeoComponentes in
// the Vulkan backend). DkImageSwizzle is VkComponentSwizzle minus one.
void Swizzle(uint32_t swizzle_guest, uint16_t swizzle_host, DkImageSwizzle salida[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t valor = (swizzle_guest >> (i * 3)) & 0x7;
    if (valor <= 3) {
      salida[i] = DkImageSwizzle(DkImageSwizzle_Red + ((swizzle_host >> (valor * 3)) & 0x7));
    } else if (valor == 4) {
      salida[i] = DkImageSwizzle_Zero;
    } else {
      salida[i] = DkImageSwizzle_One;
    }
  }
}

// The original untiling, block by block (when the block is not a power of two bytes, which no format here has).
void LeerNivelMosaicoDeSiempre(const uint8_t* origen, uint32_t pitch_bloques, uint32_t log2, uint32_t bytes, uint32_t ox,
                               uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host, uint8_t* destino) {
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const int32_t desplazamiento = DesplazamientoMosaico2D(int32_t(ox + x), int32_t(oy + y), pitch_bloques, log2);
      std::memcpy(destino + (size_t(y) * bx_host + x) * bytes, origen + desplazamiento, bytes);
    }
  }
}

// The view format for a fetch constant's TextureSign (bits 2-9 of its first word, two per component), or None to keep
// the image's. All four components signed: the Snorm form. Red, green and blue gamma (alpha gamma or unsigned): the
// sRGB form (the Xbox 360's piecewise curve, approximated by sRGB). Anything else: None (read unsigned).
DkImageFormat FormatoVistaSigno(DkImageFormat formato, uint32_t signos) {
  if (signos == 0x55) {  // 1, 1, 1, 1
    switch (formato) {
      case DkImageFormat_R8_Unorm: return DkImageFormat_R8_Snorm;
      case DkImageFormat_RG8_Unorm: return DkImageFormat_RG8_Snorm;
      case DkImageFormat_RGBA8_Unorm: return DkImageFormat_RGBA8_Snorm;
      case DkImageFormat_R16_Unorm: return DkImageFormat_R16_Snorm;
      case DkImageFormat_RG16_Unorm: return DkImageFormat_RG16_Snorm;
      case DkImageFormat_RGBA16_Unorm: return DkImageFormat_RGBA16_Snorm;
      case DkImageFormat_R_BC4_Unorm: return DkImageFormat_R_BC4_Snorm;
      case DkImageFormat_RG_BC5_Unorm: return DkImageFormat_RG_BC5_Snorm;
      default: return DkImageFormat_None;
    }
  }
  if ((signos & 0x3F) == 0x3F && ((signos >> 6) == 0 || (signos >> 6) == 3)) {
    switch (formato) {
      case DkImageFormat_RGBA8_Unorm: return DkImageFormat_RGBA8_Unorm_sRGB;
      case DkImageFormat_RGBA_BC1: return DkImageFormat_RGBA_BC1_sRGB;
      case DkImageFormat_RGBA_BC2: return DkImageFormat_RGBA_BC2_sRGB;
      case DkImageFormat_RGBA_BC3: return DkImageFormat_RGBA_BC3_sRGB;
      default: return DkImageFormat_None;
    }
  }
  return DkImageFormat_None;
}

}  // namespace

// Same host formats as the Vulkan backend (FormatoTexturaDe), as deko3d formats.
bool RenderDk::FormatoDe(uint32_t formato, FormatoDk& f) {
  switch (formato) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {DkImageFormat_R8_Unorm, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {DkImageFormat_RG8_Unorm, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 4:  // k_5_6_5 (red in the high bits, as the host's)
      f = {DkImageFormat_RGB565_Unorm, 1, 2, 2, kSwizzleRGBA};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {DkImageFormat_RGBA8_Unorm, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {DkImageFormat_RGB10A2_Unorm, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {DkImageFormat_RGBA_BC1, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {DkImageFormat_RGBA_BC2, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {DkImageFormat_RGBA_BC3, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {DkImageFormat_RG_BC5_Unorm, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {DkImageFormat_R_BC4_Unorm, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {DkImageFormat_R16_Unorm, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {DkImageFormat_RG16_Unorm, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {DkImageFormat_RGBA16_Unorm, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {DkImageFormat_R16_Float, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {DkImageFormat_RG16_Float, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {DkImageFormat_RGBA16_Float, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {DkImageFormat_R32_Float, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {DkImageFormat_RG32_Float, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {DkImageFormat_RGBA32_Float, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

// ---- Descriptors

uint32_t RenderDk::ReservarDescriptorImagen() {
  if (!descriptores_libres_.empty()) {
    const uint32_t r = descriptores_libres_.back();
    descriptores_libres_.pop_back();
    return r;
  }
  if (siguiente_descriptor_ < nucleo_.config().descriptores_imagen) {
    return siguiente_descriptor_++;
  }
  return 0;
}

void RenderDk::LiberarDescriptoresDe(uint64_t serie) {
  const auto it = vistas_por_serie_.find(serie);
  if (it == vistas_por_serie_.end()) {
    return;
  }
  auto& por_liberar = descriptores_por_liberar_[trabajo_ % descriptores_por_liberar_.size()];
  for (uint64_t clave : it->second) {
    if (const auto v = vistas_.find(clave); v != vistas_.end()) {
      por_liberar.push_back(v->second);
      vistas_.erase(v);
    }
  }
  vistas_por_serie_.erase(it);
}

void RenderDk::EscribirDescriptorImagen(uint32_t ranura, const DkImageView& vista) {
  AsegurarTrabajo();
  DkImageDescriptor d;
  dkImageDescriptorInitialize(&d, &vista, false, false);
  // In the pre-list: ordered before every draw of this submission, never under a draw that reads it (a freed slot is
  // only reused after its fence).
  dkCmdBufPushData(nucleo_.ComandosPrevios(false), nucleo_.DescriptoresImagen() + DkGpuAddr(ranura) * sizeof(d), &d,
                   sizeof(d));
}

uint32_t RenderDk::VistaDe(const ImagenDk& imagen, uint32_t swizzle_guest, uint16_t swizzle_host,
                           DkImageFormat formato_vista, bool estencil) {
  const uint64_t clave = (imagen.serie << 24) ^ (uint64_t(swizzle_guest & 0xFFF) << 12) ^ swizzle_host ^
                         (uint64_t(formato_vista) << 56) ^ (estencil ? uint64_t(1) << 55 : 0);
  if (const auto it = vistas_.find(clave); it != vistas_.end()) {
    return it->second;
  }
  const uint32_t ranura = ReservarDescriptorImagen();
  if (!ranura) {
    Avisar(35, "tabla de descriptores de imagenes llena");
    return 0;
  }
  DkImageView v;
  dkImageViewDefaults(&v, &imagen.imagen);
  Swizzle(swizzle_guest, swizzle_host, v.swizzle);
  if (formato_vista != DkImageFormat_None) {
    v.format = formato_vista;
  }
  if (imagen.profundidad) {
    v.dsSource = estencil ? DkDsSource_Stencil : DkDsSource_Depth;
  }
  EscribirDescriptorImagen(ranura, v);
  vistas_.emplace(clave, ranura);
  vistas_por_serie_[imagen.serie].push_back(clave);
  return ranura;
}

const ImagenDk* RenderDk::ResueltaConSigno(const ImagenDk& resuelta, uint32_t signos) {
  // Per host channel: the guest's X..W are R..A, or B, G, R, A with the red/blue swap of the resolve.
  int32_t modos[4] = {};
  bool alguno = false, firmado = false;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t modo = (signos >> (2 * i)) & 0x3;
    if (modo != 1 && modo != 2) continue;
    const uint32_t h = resuelta.intercambio_rb && (i == 0 || i == 2) ? 2 - i : i;
    modos[h] = int32_t(modo);
    alguno = true;
    firmado |= modo == 1;
  }
  if (!alguno || (firmado && resuelta.formato != DkImageFormat_RGBA8_Unorm)) {
    return nullptr;
  }
  if (convertidas_signo_.size() > 64) {  // resolves recreated at other sizes leave theirs behind
    for (auto& [clave, c] : convertidas_signo_) {
      DestruirImagen(c.imagen);
    }
    convertidas_signo_.clear();
  }
  ConvertidaSigno& c = convertidas_signo_[(resuelta.serie << 8) | (signos & 0xFF)];
  if (!c.imagen.viva) {
    if (!CrearImagen(c.imagen, DkImageType_2D, DkImageFormat_RGBA16_Float, resuelta.ancho, resuelta.alto, 1, 1,
                     DkImageFlags_UsageRender, "resuelta con signo")) {
      return nullptr;
    }
    REXLOG_INFO("[deko3d] resuelta {}x{} leida con signo {:02X}: copia convertida", resuelta.ancho, resuelta.alto,
                signos);
  }
  if (c.version != resuelta.version) {
    tipo_copia_ = "resuelta con signo";
    if (!Copiar3D(resuelta, 0, 0, resuelta.ancho, resuelta.alto, c.imagen, 0, 0, modos)) {
      return nullptr;
    }
    OlvidarEstado();  // the copy bound its own target and shaders; the draw binds its pass again
    c.version = resuelta.version;
    ++c.imagen.version;
  }
  return &c.imagen;
}

const ImagenDk* RenderDk::ResueltaFija16(const ImagenDk& resuelta, uint32_t signos, uint32_t canales,
                                         int32_t exp_adjust) {
  // Per channel: 4 signed (value / 32), 3 unsigned (the fixed-point bits as a normalized integer).
  int32_t modos[4] = {};
  for (uint32_t i = 0; i < canales; ++i) {
    modos[i] = ((signos >> (2 * i)) & 0x3) == 1 ? 4 : 3;
  }
  if (convertidas_fijas_.size() > 64) {
    for (auto& [clave, c] : convertidas_fijas_) {
      DestruirImagen(c.imagen);
    }
    convertidas_fijas_.clear();
  }
  ConvertidaSigno& c = convertidas_fijas_[(resuelta.serie << 20) | (uint64_t(exp_adjust & 0x3F) << 12) |
                                          (uint64_t(canales) << 8) | (signos & 0xFF)];
  if (!c.imagen.viva) {
    if (!CrearImagen(c.imagen, DkImageType_2D, DkImageFormat_RGBA16_Float, resuelta.ancho, resuelta.alto, 1, 1,
                     DkImageFlags_UsageRender, "resuelta 16_16 fija")) {
      return nullptr;
    }
    REXLOG_INFO("[deko3d] resuelta {} {}x{} leida como textura de punto fijo (signos {:02X}, exp_adjust {}): copia "
                "convertida", canales == 4 ? "16_16_16_16" : "16_16", resuelta.ancho, resuelta.alto, signos, exp_adjust);
  }
  if (c.version != resuelta.version) {
    tipo_copia_ = "resuelta fija16";
    if (!Copiar3D(resuelta, 0, 0, resuelta.ancho, resuelta.alto, c.imagen, 0, 0, modos, false,
                  std::ldexp(1.0f, exp_adjust))) {
      return nullptr;
    }
    OlvidarEstado();
    c.version = resuelta.version;
    ++c.imagen.version;
  }
  return &c.imagen;
}

const ImagenDk* RenderDk::ResueltaRecortada(const ImagenDk& resuelta, uint32_t ancho, uint32_t alto) {
  if (recortadas_.size() > 64) {  // resolves recreated at other sizes leave theirs behind
    for (auto& [clave, c] : recortadas_) {
      DestruirImagen(c.imagen);
    }
    recortadas_.clear();
  }
  ConvertidaSigno& c = recortadas_[(resuelta.serie << 28) | (uint64_t(ancho) << 14) | alto];
  if (!c.imagen.viva) {
    // A depth one is copied on the 3D engine as depth (gl_FragDepth, Copiar3D): no 2D engine use.
    const uint32_t flags =
        resuelta.profundidad ? DkImageFlags_UsageRender : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    if (!CrearImagen(c.imagen, DkImageType_2D, resuelta.formato, ancho, alto, 1, 1, flags, "resuelta recortada")) {
      return nullptr;
    }
    REXLOG_INFO("[deko3d] resuelta {}x{} leida como {}x{}: copia de su esquina", resuelta.ancho, resuelta.alto, ancho,
                alto);
  }
  c.imagen.intercambio_rb = resuelta.intercambio_rb;
  if (c.version != resuelta.version) {
    tipo_copia_ = "resuelta recortada";
    if (!Copiar3D(resuelta, 0, 0, ancho, alto, c.imagen, 0, 0)) {
      return nullptr;
    }
    OlvidarEstado();  // the copy bound its own target and shaders; the draw binds its pass again
    c.version = resuelta.version;
    ++c.imagen.version;
  }
  return &c.imagen;
}

const ImagenDk* RenderDk::ResueltaApilada(uint32_t base, const ImagenDk& primera, uint32_t alto) {
  const uint32_t ancho = primera.ancho, alto_corte = primera.alto, cortes = alto / alto_corte;
  // A 32-bit depth resolve of ancho x alto_corte texels: the next one starts right after it (tiled or not, the
  // macro tiles of 32 rows end with it when alto_corte is a multiple of 32).
  const uint32_t bytes_corte = ancho * alto_corte * 4;
  if (apiladas_.size() > 16) {
    for (auto& [clave, a] : apiladas_) {
      DestruirImagen(a.imagen);
    }
    apiladas_.clear();
  }
  ApiladaDk& a = apiladas_[(uint64_t(base) << 28) | (uint64_t(ancho) << 14) | alto];
  if (!a.imagen.viva) {
    if (!CrearImagen(a.imagen, DkImageType_2D, primera.formato, ancho, alto, 1, 1, DkImageFlags_UsageRender,
                     "resueltas apiladas")) {
      return nullptr;
    }
    // Slices not resolved yet read as the far plane, as a new resolved texture does.
    a.imagen.borrado_z = 1.0f;
    a.imagen.borrado_estencil = 0;
    a.imagen.borrado_solo_estencil = false;
    a.imagen.borrado_pendiente = true;
    HacerBorradoPendiente(a.imagen);
    std::fill(std::begin(a.series), std::end(a.series), 0);
    REXLOG_INFO("[deko3d] resueltas de profundidad {}x{} en {:08X} leidas como {}x{}: {} apiladas", ancho, alto_corte,
                base, ancho, alto, cortes);
  }
  bool copiada = false;
  for (uint32_t k = 0; k < cortes; ++k) {
    const ImagenDk* corte = k ? TexturaResuelta(base + k * bytes_corte, ancho, alto_corte) : &primera;
    if (!corte || !corte->profundidad || corte->ancho != ancho || corte->alto != alto_corte) {
      continue;
    }
    if (a.series[k] == corte->serie && a.versiones[k] == corte->version) {
      continue;
    }
    tipo_copia_ = "corte";
    if (!Copiar3D(*corte, 0, 0, ancho, alto_corte, a.imagen, 0, k * alto_corte)) {
      return nullptr;
    }
    a.series[k] = corte->serie;
    a.versiones[k] = corte->version;
    copiada = true;
  }
  if (copiada) {
    OlvidarEstado();  // the copies bound their own target and shaders; the draw binds its pass again
    ++a.imagen.version;
  }
  return &a.imagen;
}

uint32_t RenderDk::VaciaDe(uint16_t tipo_shader) const {
  return TipoVolumen(tipo_shader) ? 1 : TipoCubo(tipo_shader) ? 2 : 0;
}

bool RenderDk::CrearVacias() {
  static constexpr DkImageType kTipos[3] = {DkImageType_2D, DkImageType_3D, DkImageType_Cubemap};
  for (uint32_t i = 0; i < 3; ++i) {
    ImagenDk& v = vacias_[i];
    if (!CrearImagen(v, kTipos[i], DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, 0, "textura vacia")) {
      return false;
    }
    const deko::Subida s = Reservar(4 * 6, 256);
    if (!s) {
      return false;
    }
    std::memset(s.cpu, 0, 4 * 6);
    DkImageView vista;
    dkImageViewDefaults(&vista, &v.imagen);
    const DkCopyBuf origen = {s.gpu, 4, 4};
    const DkImageRect rect = {0, 0, 0, 1, 1, i == 2 ? 6u : 1u};
    dkCmdBufCopyBufferToImage(nucleo_.ComandosPrevios(), &origen, &vista, &rect, 0);
    EscribirDescriptorImagen(i, vista);
  }
  // fc3-switch 2026-11-09 DIAGNOSTIC: a white 1x1 2D texture in its own descriptor (slot 3 is the output pass's).
  {
    ranura_blanca_ = ReservarDescriptorImagen();
    if (!CrearImagen(blanca_, DkImageType_2D, DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, 0, "textura blanca")) {
      return false;
    }
    const deko::Subida s = Reservar(4, 256);
    if (!s) {
      return false;
    }
    std::memset(s.cpu, 0xFF, 4);
    DkImageView vista;
    dkImageViewDefaults(&vista, &blanca_.imagen);
    const DkCopyBuf origen = {s.gpu, 4, 4};
    const DkImageRect rect = {0, 0, 0, 1, 1, 1};
    dkCmdBufCopyBufferToImage(nucleo_.ComandosPrevios(), &origen, &vista, &rect, 0);
    EscribirDescriptorImagen(ranura_blanca_, vista);
  }
  // Sampler 0: bilinear, clamped (the output pass).
  DkSampler s;
  dkSamplerDefaults(&s);
  s.minFilter = s.magFilter = DkFilter_Linear;
  s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  dkCmdBufPushData(nucleo_.ComandosPrevios(false), nucleo_.DescriptoresSampler(), &d, sizeof(d));
  siguiente_descriptor_ = kDescriptoresReservados;
  return true;
}

// The sampler of a fetch constant, as the Vulkan backend builds it (RanuraSampler), without its measurement tests.
// `puntual`: no filtering (depth textures).
uint32_t RenderDk::SamplerDe(const uint32_t* f, bool puntual) {
  const uint32_t campo_filtros = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
  const uint32_t campo_mips = (f[4] >> 2) & 0xFF;       // minimum in 0-3, maximum in 4-7
  // With the fetch constant's LOD bias (dword 4 bits 12-21, signed, in 1/32: Gears of War 2's level-2 materials).
  const uint32_t sesgo_lod = (f[4] >> 12) & 0x3FF;
  const uint64_t clave = ((f[0] >> 10) & 0x1FF) | (uint64_t(campo_filtros) << 9) | (uint64_t(campo_mips) << 21) |
                         (uint64_t(f[5] & 0x3) << 29) | (puntual ? 0x80000000ull : 0ull) | (uint64_t(sesgo_lod) << 32);
  if (const auto it = samplers_.find(clave); it != samplers_.end()) {
    return it->second;
  }
  if (siguiente_sampler_ >= nucleo_.config().descriptores_sampler) {
    Avisar(37, "tabla de samplers llena");
    return 0;
  }
  // The Vulkan backend's address modes: the halfway clamps as edge clamps, the mirrored ones as mirror-to-edge.
  static constexpr DkWrapMode kModos[8] = {
      DkWrapMode_Repeat,      DkWrapMode_MirroredRepeat,    DkWrapMode_ClampToEdge,   DkWrapMode_MirrorClampToEdge,
      DkWrapMode_ClampToEdge, DkWrapMode_MirrorClampToEdge, DkWrapMode_ClampToBorder, DkWrapMode_MirrorClampToEdge};
  DkSampler s;
  dkSamplerDefaults(&s);
  s.magFilter = ((f[3] >> 19) & 0x3) == 1 && !puntual ? DkFilter_Linear : DkFilter_Nearest;
  s.minFilter = ((f[3] >> 21) & 0x3) == 1 && !puntual ? DkFilter_Linear : DkFilter_Nearest;
  const uint32_t mip = (campo_filtros >> 4) & 0x3;
  s.mipFilter = mip == 1 && !puntual ? DkMipFilter_Linear : DkMipFilter_Nearest;
  s.wrapMode[0] = kModos[(f[0] >> 10) & 0x7];
  s.wrapMode[1] = kModos[(f[0] >> 13) & 0x7];
  s.wrapMode[2] = kModos[(f[0] >> 16) & 0x7];
  s.lodClampMin = float(campo_mips & 0xF);
  // With real mips, the fetch constant's maximum level limits too; a basemap filter stays on the base level.
  s.lodClampMax = mip == 2 ? s.lodClampMin + 0.25f : std::max(s.lodClampMin, float((f[4] >> 6) & 0xF));
  s.lodBias = float(int32_t(sesgo_lod << 22) >> 22) / 32.0f;
  if ((f[5] & 0x3) == 1) {  // opaque white; otherwise transparent black
    for (auto& c : s.borderColor) {
      c.value_f = 1.0f;
    }
  }
  static const uint32_t aniso = uint32_t(std::max(0, int32_t(REXCVAR_GET(nfsmw_nativo_anisotropico))));
  if (aniso > 1 && s.minFilter == DkFilter_Linear && s.mipFilter == DkMipFilter_Linear &&
      s.lodClampMax > s.lodClampMin) {
    s.maxAnisotropy = float(std::min(aniso, 16u));
  }
  const uint32_t ranura = siguiente_sampler_++;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  AsegurarTrabajo();
  dkCmdBufPushData(nucleo_.ComandosPrevios(false), nucleo_.DescriptoresSampler() + DkGpuAddr(ranura) * sizeof(d), &d,
                   sizeof(d));
  samplers_.emplace(clave, ranura);
  return ranura;
}

// ---- Untiling (the Vulkan backend's LeerNivel and LeerVolumen, with the fast untiling always on: it was checked
// against the block-by-block one on the PC and the console)

bool RenderDk::LeerNivel(uint64_t direccion, bool mosaico, uint32_t pitch_bloques, uint64_t fila_bytes,
                         const FormatoDk& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                         uint8_t* destino) {
  if (!bx || !by) {
    return true;
  }
  const uint32_t log2 = Log2Bytes(tf.bytes);
  const uint64_t extremo =
      mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx + 31) & ~31u),
                                                                   int32_t((oy + by + 31) & ~31u), pitch_bloques, log2),
                                           0))
              : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
  if (direccion + extremo > kMemoriaFisica) {
    return false;
  }
  const uint8_t* origen = memoria_->TranslatePhysical(uint32_t(direccion));
  if (!mosaico) {
    for (uint32_t y = 0; y < by; ++y) {
      std::memcpy(destino + size_t(y) * bx_host * tf.bytes,
                  origen + uint64_t(oy + y) * fila_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
    }
    return true;
  }
  if (tf.bytes == (1u << log2)) {
    switch (log2) {
      case 0: DesenmosaicarNivel<0>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 1: DesenmosaicarNivel<1>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 2: DesenmosaicarNivel<2>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 3: DesenmosaicarNivel<3>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      default: DesenmosaicarNivel<4>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
    }
    return true;
  }
  LeerNivelMosaicoDeSiempre(origen, pitch_bloques, log2, tf.bytes, ox, oy, bx, by, bx_host, destino);
  return true;
}

bool RenderDk::LeerVolumen(const uint32_t* f, const FormatoDk& tf, uint32_t bloques_x, uint32_t bloques_y,
                           uint32_t fondo, uint32_t bloques_x_host, uint32_t bloques_y_host, uint32_t pitch_bloques,
                           bool mosaico, uint8_t* datos) {
  const uint64_t direccion = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
  const uint32_t log2 = Log2Bytes(tf.bytes);
  const uint64_t fila = uint64_t((pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes;
  const uint64_t corte = fila * ((bloques_y + 31) & ~uint32_t(31));
  const uint64_t extremo =
      mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico3D(int32_t((bloques_x + 31) & ~31u),
                                                                   int32_t((bloques_y + 31) & ~31u),
                                                                   int32_t((fondo + 3) & ~3u), pitch_bloques, bloques_y,
                                                                   log2),
                                           0))
              : corte * fondo;
  if (direccion + extremo > kMemoriaFisica) {
    return false;
  }
  const uint8_t* origen = memoria_->TranslatePhysical(uint32_t(direccion));
  const size_t bytes_corte = size_t(bloques_x_host) * bloques_y_host * tf.bytes;
  for (uint32_t z = 0; z < fondo; ++z) {
    for (uint32_t y = 0; y < bloques_y; ++y) {
      for (uint32_t x = 0; x < bloques_x; ++x) {
        const uint64_t desplazamiento =
            mosaico ? uint64_t(DesplazamientoMosaico3D(int32_t(x), int32_t(y), int32_t(z), pitch_bloques, bloques_y, log2))
                    : z * corte + y * fila + uint64_t(x) * tf.bytes;
        std::memcpy(datos + bytes_corte * z + (size_t(y) * bloques_x_host + x) * tf.bytes, origen + desplazamiento,
                    tf.bytes);
      }
    }
  }
  return true;
}

// ---- The texture of a fetch constant

uint32_t RenderDk::PrepararTextura(const uint32_t* f, uint16_t tipo_shader, uint32_t& ancho_out, uint32_t& alto_out,
                                   bool& puntual, uint64_t& valido_hasta, uint32_t& sesgo_sin_convertir) {
  sesgo_sin_convertir = 0;
  ancho_out = 0;  // 0 = unknown (1/size is not written)
  alto_out = 0;
  puntual = false;
  valido_hasta = fotograma_;
  if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
    return VaciaDe(tipo_shader);
  }
  const uint32_t dimension = (f[5] >> 9) & 0x3;
  bool cubo = dimension == uint32_t(xenos::DataDimension::kCube);
  const bool volumen = dimension == uint32_t(xenos::DataDimension::k3D);
  // A cube read by a 2D fetch: on the Xenos that reads the first face (it starts at the base address with a 2D
  // texture's layout). Dead Space 2 does it; an empty texture made what it draws invisible. Base level only (the
  // faces' mips are laid out differently), and its own cache key.
  bool cara0 = false;
  if (cubo && !TipoCubo(tipo_shader) && !TipoVolumen(tipo_shader)) {
    cubo = false;
    cara0 = true;
    Avisar(39, "cubo leido como 2D: se usa su primera cara");
  }
  if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cubo && !volumen) {
    Avisar(30, "texturas 1D todavia no soportadas: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  if (TipoVolumen(tipo_shader) != volumen || TipoCubo(tipo_shader) != cubo) {
    Avisar(29, "la dimension de la textura no es la que declara el shader: se usa una vacia");
    // Which ones (the warning above is once): a few distinct ones with what the texture and the shader say.
    static uint32_t detalles = 0;
    if (detalles < 16) {
      ++detalles;
      REXLOG_WARN("[deko3d] causa 29: textura {:08X} dimension {} ({}x{}, formato {}), el shader declara tipo {}",
                  (f[1] >> 12) << 12, dimension, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, f[1] & 0x3F,
                  tipo_shader);
    }
    return VaciaDe(tipo_shader);
  }
  // TextureSign of each component (x, y, z, w at bits 2, 4, 6, 8): 0 unsigned, 1 signed, 2 unsigned biased (2x - 1),
  // 3 gamma. All signed, or all gamma, is done with a view in the matching host format (FormatoVistaSigno); other mixes
  // are read unsigned, as before.
  const uint32_t signos = (f[0] >> 2) & 0xFF;
  const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
  const uint32_t base = (f[1] >> 12) << 12;
  if (!cubo && !volumen && REXCVAR_GET(fc3_dk_pequenas_blancas) && ((f[2] & 0x1FFF) + 1) <= 4 &&
      (((f[2] >> 13) & 0x1FFF) + 1) <= 4) {
    return ranura_blanca_;  // DIAGNOSTIC: every texture of 4x4 texels or less reads white
  }
  if (!cubo && !volumen) {
    const uint32_t ancho_fetch = (f[2] & 0x1FFF) + 1, alto_fetch = ((f[2] >> 13) & 0x1FFF) + 1;
    if (const ImagenDk* resuelta = TexturaResuelta(base & 0x1FFFFFFF, ancho_fetch, alto_fetch)) {
      // Read taller than it is, at its own width: depth resolves stacked in memory (Dead Space 3's shadow cascades).
      // Sampled alone, the first cascade was stretched over all of them: wrong sun shadows everywhere.
      if (resuelta->profundidad && ancho_fetch == resuelta->ancho && alto_fetch > resuelta->alto &&
          alto_fetch % resuelta->alto == 0 && alto_fetch / resuelta->alto <= 4) {
        if (const ImagenDk* apilada = ResueltaApilada(base & 0x1FFFFFFF, *resuelta, alto_fetch)) {
          resuelta = apilada;
          valido_hasta = fotograma_ ? fotograma_ - 1 : 0;  // looked at again on every draw, as the corners below
        }
      }
      // Read smaller than it is: its corner of the fetch's size (Dead Space 2's 320x180 blur levels live in
      // 640-wide textures; sampled whole, the final image came out in stretched vertical strips). Depth too: Dead
      // Space 3's half-size depth is resolved 640 wide and read as 576x360; sampled whole, its ambient occlusion and
      // depth of field were squeezed to 0.9 of the screen (outlines of the characters beside them).
      if (ancho_fetch <= resuelta->ancho && alto_fetch <= resuelta->alto &&
          (ancho_fetch < resuelta->ancho || alto_fetch < resuelta->alto)) {
        if (const ImagenDk* recortada = ResueltaRecortada(*resuelta, ancho_fetch, alto_fetch)) {
          resuelta = recortada;
          // Looked at again on every draw: a resolve later in the frame must reach the copy.
          valido_hasta = fotograma_ ? fotograma_ - 1 : 0;
        }
      }
      // Resolved textures report their size like any other (without it the shadow PCF taps collapse).
      ancho_out = resuelta->ancho;
      alto_out = resuelta->alto;
      if (resuelta->profundidad) {
        // fc3-switch (PROTOTYPE): fetched as a color format (k_8_8_8_8): the hardware returns the D24S8 word's bytes,
        // stencil in X. Far Cry 3's pre-lighting passes classify pixels by that stencil (.z after the ARGB swizzle);
        // with depth in every channel no light was drawn. Asking here makes the next resolves fill the RGBA8 copy.
        const uint32_t formato_fetch = f[1] & 0x3F;
        if (formato_fetch != 22 && formato_fetch != 23) {
          ZComoColor& c = z_como_color_[base & 0x1FFFFFFF];
          if (c.escrita && c.imagen.ancho == ancho_fetch && c.imagen.alto == alto_fetch) {
            puntual = true;
            return VistaDe(c.imagen, swizzle, kSwizzleRGBA);
          }
          Avisar(42, "profundidad resuelta leida como color sin su copia con estencil todavia: se lee la profundidad");
        }
        // A depth copy (k_24_8): depth in R, which the fetch swizzle distributes; read unfiltered.
        puntual = true;
        return VistaDe(*resuelta, swizzle, kSwizzleRRRR);
      }
      // The emulation's copy swaps channels in memory (copy_dest_swap) and the fetch swizzle undoes it; here the copy
      // is image to image, so the swap goes into the host channels.
      const uint16_t host = resuelta->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA;
      // A k_16_16 target (fixed point, -32..32) read back as a k_16_16 texture, as the hardware's bits would read
      // (unsigned: about value / 64; signed: value / 32) times 2^exp_adjust of the fetch constant (dword 3, bits 13-18).
      // Off by default: Gears of War 2 and Dead Space 3 read the value they wrote. (Dead Space 3's deferred lighting
      // buffer, k_16_16_16_16, is scaled by its resolve's copy_dest_exp_bias of -5 instead, which the copy applies;
      // converting it here as well made the scene black.)
      const int32_t exp_adjust = int32_t(f[3] << 13) >> 26;  // bits 13-18, signed
      // The 10:10:10:2 HDR resolve is sampled with exp_adjust +2/+3 by FC3's post shaders.
      // Zero fixed-point channels leaves the normalized components intact; the float copy applies the fetch scale.
      if ((f[1] & 0x3F) == 54 && resuelta->formato == DkImageFormat_RGB10A2_Unorm && !signos && exp_adjust) {
        if (const ImagenDk* scaled = ResueltaFija16(*resuelta, 0, 0, exp_adjust)) {
          valido_hasta = fotograma_ ? fotograma_ - 1 : 0;
          return VistaDe(*scaled, swizzle, host);
        }
      }
      // fc3-switch 2026-11-09 (fc3_dk_exp_adjust_rgba8): FC3's FXAA 360 pass (PS 2382) reads its 8_8_8_8 input through
      // three fetch constants with exp_adjust 0, -1 and -2 (x1, x0.5, x0.25) and adds the scaled taps as averages;
      // ignored, the edge taps came out 2-4x too bright: outlines on every edge. A scaled copy, as for the HDR above.
      // fc3_dk_exp_adjust_float16 (2026-11-09): the same for 16_16_16_16_FLOAT resolves: FC3 resolves its half-res HDR
      // (640x352) with copy_dest_exp_bias +11 and reads it with exp_adjust -14; only the +11 was applied (x16384).
      const bool escalar_float16 = (f[1] & 0x3F) == 26 && resuelta->formato == DkImageFormat_RGBA16_Float && !signos &&
                                   exp_adjust && REXCVAR_GET(fc3_dk_exp_adjust_float16);
      if (escalar_float16) {
        if (const ImagenDk* scaled = ResueltaFija16(*resuelta, 0, 0, exp_adjust)) {
          valido_hasta = fotograma_ ? fotograma_ - 1 : 0;
          return VistaDe(*scaled, swizzle, host);
        }
      }
      if ((f[1] & 0x3F) == 6 && resuelta->formato == DkImageFormat_RGBA8_Unorm && !signos && exp_adjust &&
          REXCVAR_GET(fc3_dk_exp_adjust_rgba8)) {
        if (const ImagenDk* scaled = ResueltaFija16(*resuelta, 0, 0, exp_adjust)) {
          valido_hasta = fotograma_ ? fotograma_ - 1 : 0;
          return VistaDe(*scaled, swizzle, host);
        }
      }
      if ((f[1] & 0x3F) == 25 && resuelta->formato == DkImageFormat_RG16_Float && REXCVAR_GET(nfsmw_dk_16_16_fijo)) {
        if (const ImagenDk* convertida = ResueltaFija16(*resuelta, signos, 2, exp_adjust)) {
          return VistaDe(*convertida, swizzle, host);
        }
      }
      if (signos) {
        // Signed or biased channels: a converted copy (Dead Space 2's normal buffer is written as n * 0.5 + 0.5 into an
        // 8_8_8_8 target and read back biased, so its lights had every normal positive). All gamma: an sRGB view.
        if (const ImagenDk* convertida = ResueltaConSigno(*resuelta, signos)) {
          return VistaDe(*convertida, swizzle, host);
        }
        return VistaDe(*resuelta, swizzle, host, FormatoVistaSigno(resuelta->formato, signos));
      }
      return VistaDe(*resuelta, swizzle, host);
    }
  }
  const uint32_t formato = f[1] & 0x3F;
  FormatoDk tf;
  if (!FormatoDe(formato, tf)) {
    Avisar(400 + formato, "formato de textura todavia no soportado: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  const DkImageFormat formato_vista = FormatoVistaSigno(tf.formato, signos);
  if (signos && formato_vista == DkImageFormat_None) {
    Avisar(31, "texturas con signo o gamma mezclados o sesgados: se leen sin signo");
    // Record the actual fetch, not only the once-per-cause warning, so that a
    // reference capture can identify which material and channels are affected.
    static uint32_t detalles_signo = 0;
    if (detalles_signo < 16) {
      ++detalles_signo;
      REXLOG_WARN("[deko3d] causa 31: base {:08X} formato {} signos {:02X} swizzle {:03X} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  base, formato, signos, swizzle, f[0], f[1], f[2], f[3], f[4], f[5]);
    }
  }
  // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h).
  const uint32_t ancho = volumen ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
  const uint32_t alto = volumen ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t fondo = volumen ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
  if (volumen && tf.bloque > 1) {
    Avisar(38, "textura 3D comprimida: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  const uint32_t capas = cubo ? 6 : 1;
  const uint32_t bloques_x = (ancho + tf.bloque - 1) / tf.bloque;
  const uint32_t bloques_y = (alto + tf.bloque - 1) / tf.bloque;
  const uint32_t pitch_bloques = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.bloque, 1);
  // Cubemap faces are consecutive: each takes its base level with rows and columns aligned to 32 blocks and the
  // total to 4 KB (GetGuestTextureLayout).
  const uint64_t zancada_cara =
      (uint64_t((pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes * ((bloques_y + 31) & ~uint32_t(31)) + 4095) &
      ~uint64_t(4095);
  const uint32_t claves[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, (f[5] >> 9) | (uint32_t(cara0) << 31)};
  const uint64_t clave = XXH3_64bits(claves, sizeof(claves));

  // Mip levels with the rules of GetSubresourcesFromFetchConstant: no mip address, no mips; the maximum clamped to the
  // size; without the base or with a minimum above 0, the base is not read. 3D textures: base only.
  static const bool mipmaps = REXCVAR_GET(nfsmw_nativo_mipmaps);
  const bool mosaico = (f[0] >> 31) & 0x1;
  const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
  const uint64_t dir_mips = cara0 ? 0 : uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
  const uint32_t nivel_empaquetado =
      mipmaps && !volumen && ((f[5] >> 11) & 0x1) ? NivelEmpaquetado(ancho, alto) : UINT32_MAX;
  uint32_t nivel_max = 0;
  bool leer_base = true;
  if (mipmaps && !volumen && dir_mips != 0) {
    const uint32_t tam_max = Log2Suelo(std::max(ancho, alto));
    uint32_t nivel_min = std::min((f[4] >> 2) & 0xFu, tam_max);
    nivel_max = std::max(std::min((f[4] >> 6) & 0xFu, tam_max), nivel_min);
    if (nivel_max != 0) {
      if (dir_base == 0) {
        nivel_min = std::max(nivel_min, 1u);
      }
      leer_base = nivel_min == 0;
    }
  }
  // Mip regions from dir_mips (GetGuestTextureLayout): rows of 32 blocks from the size rounded up to a power of 2
  // (and 256-byte rows if linear), layers aligned to 4 KB; the packed tail's levels share its first region.
  struct RegionMip {
    uint64_t desplazamiento = 0;
    uint64_t fila_bytes = 0;
    uint64_t zancada = 0;
    uint32_t pitch_bloques = 0;
  };
  std::array<RegionMip, 16> regiones{};
  uint64_t extension_mips = 0;
  if (nivel_max != 0) {
    const uint32_t ultima = nivel_empaquetado == 0 ? 0 : std::min(nivel_max, nivel_empaquetado);
    for (uint32_t s = nivel_empaquetado == 0 ? 0 : 1; s <= ultima; ++s) {
      RegionMip& region = regiones[s];
      const uint32_t fila_texels = std::max(std::bit_ceil(ancho) >> s, 1u);
      const uint32_t filas_texels = std::max(std::bit_ceil(alto) >> s, 1u);
      region.pitch_bloques = ((fila_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u;
      region.fila_bytes = uint64_t(region.pitch_bloques) * tf.bytes;
      if (!mosaico) {
        region.fila_bytes = (region.fila_bytes + 255) & ~uint64_t(255);
      }
      const uint64_t filas_bloques = uint64_t(((filas_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u);
      region.zancada = (region.fila_bytes * filas_bloques + 4095) & ~uint64_t(4095);
      region.desplazamiento = extension_mips;
      extension_mips += region.zancada * capas;
    }
    if (dir_mips + extension_mips > kMemoriaFisica) {
      nivel_max = 0;  // the mips would go past memory: base only
      extension_mips = 0;
      leer_base = true;
    }
  }
  // With the packed tail from level 0 (short side of 16 texels or less), the base does not start at its address
  // either.
  uint32_t base_ox = 0;
  uint32_t base_oy = 0;
  if (nivel_empaquetado == 0 && !REXCVAR_GET(fc3_dk_cola_base_en_origen)) {
    DesplazamientoEmpaquetado(ancho, alto, tf.bloque, 0, base_ox, base_oy);
  }

  // The game's dynamic cubemaps (the car's environment, the rear-view mirror's face): their faces are resolves to
  // consecutive addresses, so the guest memory never holds them. Each face is copied from its resolved texture into
  // the cube's layer once per frame, at the first draw that samples it (the Vulkan path does the same in its upload
  // command buffer, a frame later).
  if (cubo) {
    std::array<const ImagenDk*, 6> caras{};
    bool resueltas = true;
    for (uint32_t c = 0; c < 6 && resueltas; ++c) {
      caras[c] = TexturaResuelta(uint32_t((dir_base + c * zancada_cara) & 0x1FFFFFFF));
      resueltas = caras[c] && !caras[c]->profundidad && caras[c]->formato == DkImageFormat_RGBA8_Unorm &&
                  caras[c]->ancho >= ancho && caras[c]->alto >= alto;
    }
    if (resueltas) {
      TexturaDk& cubo_resuelto = texturas_[clave ^ 0x9E3779B97F4A7C15ull];
      if (!cubo_resuelto.imagen.viva) {
        if (!CrearImagen(cubo_resuelto.imagen, DkImageType_Cubemap, DkImageFormat_RGBA8_Unorm, ancho, alto, 1, 1, 0,
                         "cubo de caras resueltas")) {
          texturas_.erase(clave ^ 0x9E3779B97F4A7C15ull);
          Avisar(32, "no se pudo crear una textura");
          return VaciaDe(tipo_shader);
        }
        cubo_resuelto.bytes = uint64_t(ancho) * alto * 4 * 6;
        bytes_texturas_ += cubo_resuelto.bytes;
        ++texturas_creadas_;
        REXLOG_INFO("[deko3d] cubo {:08X} {}x{} con sus caras resueltas", base, ancho, alto);
      }
      if (cubo_resuelto.fotograma != fotograma_) {
        cubo_resuelto.fotograma = fotograma_;
        // Only the faces resolved since the last refresh (the game redraws one face per frame or fewer). Transfers
        // on the copy engine: no 3D state changes, so the pass and a deferred sky stay as they are.
        bool alguna = false;
        DkImageView destino;
        dkImageViewDefaults(&destino, &cubo_resuelto.imagen.imagen);
        for (uint32_t c = 0; c < 6; ++c) {
          if (cubo_resuelto.caras_version[c] == caras[c]->version + 1) {
            continue;  // stored as version + 1, so a new cube (all 0) copies every face
          }
          cubo_resuelto.caras_version[c] = caras[c]->version + 1;
          if (!alguna) {
            alguna = true;
            MarcarGpu(kGpuCopias);
            AntesDeTransferir();
          }
          DkImageView origen;
          dkImageViewDefaults(&origen, &caras[c]->imagen);
          const DkImageRect a = {0, 0, 0, ancho, alto, 1};
          const DkImageRect b = {0, 0, c, ancho, alto, 1};
          dkCmdBufCopyImage(nucleo_.comandos(), &origen, &a, &destino, &b, 0);
        }
        if (alguna) {
          ++cubos_refrescados_;
          if (g_.pase_valido) {
            MarcarGpu(categoria_pase_);
          }
        }
      }
      ancho_out = cubo_resuelto.imagen.ancho;
      alto_out = cubo_resuelto.imagen.alto;
      return VistaDe(cubo_resuelto.imagen, swizzle, caras[0]->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA);
    }
  }

  // A volume whose slices were resolved (Copiar keeps slice z at base + z * width * height * 4): its 3D image is made
  // from them, each changed slice copied into its layer once per frame, as the cubemaps of resolved faces above.
  if (volumen) {
    // What the 3D fetches look like, a few distinct ones: to see whether they are the volumes resolved by slices.
    static std::vector<uint64_t> vistos;
    const uint64_t firma = dir_base ^ (uint64_t(ancho) << 32) ^ (uint64_t(alto) << 44) ^ (uint64_t(fondo) << 54);
    if (vistos.size() < 16 && std::find(vistos.begin(), vistos.end(), firma) == vistos.end()) {
      vistos.push_back(firma);
      uint32_t encontradas = 0;
      for (uint32_t z = 0; z < fondo && z < 64; ++z) {
        encontradas += TexturaResuelta(uint32_t((dir_base + uint64_t(z) * ancho * alto * 4) & 0x1FFFFFFF)) != nullptr;
      }
      REXLOG_INFO("[deko3d] volumen muestreado {:08X} {}x{}x{} formato {} {}, mips {:08X}; capas resueltas en "
                  "base + z * {}: {} de {}",
                  base, ancho, alto, fondo, formato, mosaico ? "en mosaico" : "lineal", uint32_t(dir_mips),
                  ancho * alto * 4, encontradas, fondo);
    }
  }
  if (volumen && fondo > 1 && fondo <= 64) {
    std::vector<const ImagenDk*> capas(fondo);
    bool resueltas = true;
    for (uint32_t z = 0; z < fondo && resueltas; ++z) {
      capas[z] = TexturaResuelta(uint32_t((dir_base + uint64_t(z) * ancho * alto * 4) & 0x1FFFFFFF));
      resueltas = capas[z] && !capas[z]->profundidad && capas[z]->formato == DkImageFormat_RGBA8_Unorm &&
                  capas[z]->ancho >= ancho && capas[z]->alto >= alto;
    }
    if (resueltas) {
      VolumenResuelto& v = volumenes_resueltos_[clave];
      if (!v.imagen.viva) {
        if (!CrearImagen(v.imagen, DkImageType_3D, DkImageFormat_RGBA8_Unorm, ancho, alto, fondo, 1, 0,
                         "volumen de capas resueltas")) {
          volumenes_resueltos_.erase(clave);
          Avisar(32, "no se pudo crear una textura");
          return VaciaDe(tipo_shader);
        }
        v.versiones.assign(fondo, 0);
        REXLOG_INFO("[deko3d] volumen {:08X} {}x{}x{} con sus capas resueltas", base, ancho, alto, fondo);
      }
      if (v.fotograma != fotograma_) {
        v.fotograma = fotograma_;
        bool alguna = false;
        DkImageView destino;
        dkImageViewDefaults(&destino, &v.imagen.imagen);
        for (uint32_t z = 0; z < fondo; ++z) {
          if (v.versiones[z] == capas[z]->version + 1) {
            continue;  // stored as version + 1, so a new volume copies every slice
          }
          v.versiones[z] = capas[z]->version + 1;
          if (!alguna) {
            alguna = true;
            MarcarGpu(kGpuCopias);
            AntesDeTransferir();
          }
          DkImageView origen;
          dkImageViewDefaults(&origen, &capas[z]->imagen);
          const DkImageRect a = {0, 0, 0, ancho, alto, 1};
          const DkImageRect b = {0, 0, z, ancho, alto, 1};
          dkCmdBufCopyImage(nucleo_.comandos(), &origen, &a, &destino, &b, 0);
        }
        if (alguna && g_.pase_valido) {
          MarcarGpu(categoria_pase_);
        }
      }
      ancho_out = ancho;
      alto_out = alto;
      return VistaDe(v.imagen, swizzle, capas[0]->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA);
    }
  }

  TexturaDk& textura = texturas_[clave];
  if (!textura.imagen.viva) {
    // BC: the size in whole blocks, as the Vulkan backend (the shaders' 1/size is the host size).
    const uint32_t ancho_host = tf.bloque > 1 ? (ancho + 3) & ~uint32_t(3) : ancho;
    const uint32_t alto_host = tf.bloque > 1 ? (alto + 3) & ~uint32_t(3) : alto;
    const uint32_t niveles = std::min(nivel_max + 1, Log2Suelo(std::max(ancho_host, alto_host)) + 1);
    if (!CrearImagen(textura.imagen, cubo ? DkImageType_Cubemap : volumen ? DkImageType_3D : DkImageType_2D,
                     tf.formato, ancho_host, alto_host, volumen ? fondo : 1, niveles, 0, "textura")) {
      texturas_.erase(clave);
      Avisar(32, "no se pudo crear una textura");
      return VaciaDe(tipo_shader);
    }
    for (uint32_t n = 0; n < niveles; ++n) {
      textura.bytes += uint64_t((std::max(ancho_host >> n, 1u) + tf.bloque - 1) / tf.bloque) *
                       ((std::max(alto_host >> n, 1u) + tf.bloque - 1) / tf.bloque) * tf.bytes * capas *
                       (fondo ? fondo : 1);
    }
    bytes_texturas_ += textura.bytes;
    ++texturas_creadas_;
    // fc3-switch 2026-11-08 DIAGNOSTIC: big textures and their mip fields (owner: shimmering / "stretched" textures).
    static uint32_t grandes = 0;
    if (std::max(ancho, alto) >= 256 && grandes < 300) {
      ++grandes;
      REXLOG_INFO("[mips] {:08X} {}x{} fmt {} niveles {} f4 min {} max {} dir_mips {:08X} empaquetado {} leer_base {}",
                  base, ancho, alto, formato, niveles, (f[4] >> 2) & 0xF, (f[4] >> 6) & 0xF, uint32_t(dir_mips),
                  (f[5] >> 11) & 0x1, leer_base);
    }
    if (texturas_creadas_ <= 48) {
      REXLOG_INFO("[deko3d] textura {:08X} {}x{} formato {} {} orden {} pitch {} swizzle {:03X}{}; niveles {}", base,
                  ancho, alto, formato, mosaico ? "en mosaico" : "lineal", (f[1] >> 6) & 0x3,
                  ((f[0] >> 22) & 0x1FF) << 5, swizzle, cubo ? " (cubo)" : volumen ? " (3D)" : "", niveles);
    }
  }
  const uint32_t ranura = VistaDe(textura.imagen, swizzle, tf.swizzle_host, formato_vista);
  // Until its next check nothing about it changes: the draws' per-fetch cache keeps the answer that long.
  auto listo = [&]() {
    valido_hasta = textura.siguiente > fotograma_ ? textura.siguiente - 1 : fotograma_;
    // Experiment: regular 2D BC1 only. Resolved images returned above may
    // already have had their signs applied and must not be transformed twice.
    // fc3_sesgo_todos_formatos (EXPERIMENT, BX3): every regular (not resolved) COMPRESSED 2D texture: DXT2/3, DXT4/5, DXN,
    // DXT5A and their AS_16 forms (BX1 also biased uncompressed formats and broke the image). BC1 as before.
    static const bool todos = REXCVAR_GET(fc3_sesgo_todos_formatos);
    if (ranura && (formato == 18 || (todos && (formato == 19 || formato == 20 || formato == 49 || formato == 51 ||
                                                formato == 52 || formato == 53 || formato == 59))) && !cubo && !cara0 && !volumen && formato_vista == DkImageFormat_None) {
      sesgo_sin_convertir = MascaraSesgoTextura(signos, swizzle);
    }
    return ranura;
  };
  ancho_out = textura.imagen.ancho;
  alto_out = textura.imagen.alto;
  if (textura.fotograma == fotograma_) {
    return listo();  // already checked this frame
  }
  if (textura.con_contenido && fotograma_ < textura.siguiente) {
    return listo();
  }
  textura.fotograma = fotograma_;
  // 2D and cubemaps: the guest bytes are fingerprinted before untiling; unchanged, nothing to do.
  if (!volumen) {
    const uint32_t log2_bloque = Log2Bytes(tf.bytes);
    const uint32_t bloques_x_leidos = bloques_x + base_ox;
    const uint32_t bloques_y_leidos = bloques_y + base_oy;
    const uint64_t extension_capa =
        mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((bloques_x_leidos - 1) & ~31u),
                                                                     int32_t((bloques_y_leidos - 1) & ~31u),
                                                                     pitch_bloques, log2_bloque),
                                             0)) +
                      (log2_bloque == 0 ? 0xA00u : log2_bloque == 1 ? 0xC00u : (0x400u << log2_bloque))
                : uint64_t(std::max(pitch_bloques, bloques_x_leidos)) * tf.bytes * (bloques_y_leidos - 1) +
                      uint64_t(bloques_x_leidos) * tf.bytes;
    const uint64_t extension = (capas - 1) * zancada_cara + extension_capa;
    if (dir_base + extension <= kMemoriaFisica) {
      // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
      auto sin_cambios = [&] {
        textura.intervalo = textura.nivel_vacio ? 1 : std::min<uint32_t>(textura.intervalo * 2, 32);
        textura.siguiente = fotograma_ + textura.intervalo + (textura.intervalo == 32 ? (clave >> 7) & 7 : 0);
        return listo();
      };
      if (muestreo_huellas_ < 0) {
        const int32_t cada = REXCVAR_GET(nfsmw_nativo_huellas_muestreo);
        muestreo_huellas_ = cada >= 2 ? std::min<int32_t>(cada, 64) : 0;
        presupuesto_huellas_ = uint64_t(std::max(REXCVAR_GET(nfsmw_nativo_huellas_kb_fotograma), 0)) * 1024;
        REXLOG_INFO("[deko3d] texturas estables: recomprobacion por muestreo {}; presupuesto {} KB por fotograma",
                    muestreo_huellas_ ? fmt::format("SI, 1 de cada {} completa, las {} primeras con las dos huellas",
                                                    muestreo_huellas_, kMuestrasAComprobar)
                                      : std::string("no"),
                    presupuesto_huellas_ >> 10);
      }
      // The sampled recheck (the Vulkan path's C3, nfsmw_nativo_huellas_muestreo): a stable texture (interval 8 or
      // more) whose sample matches is taken as unchanged; 1 in N of its rechecks is still whole, and the first
      // kMuestrasAComprobar compute both hashes (one disagreement switches sampling off). Not for textures whose
      // sample would read more than half of their bytes.
      const uint64_t bytes_completa = extension + extension_mips;
      const uint64_t bytes_de_muestra = BytesMuestra(extension) + BytesMuestra(extension_mips);
      const bool con_muestra = muestreo_huellas_ > 0 && textura.con_contenido && textura.intervalo >= 4 &&
                               !textura.nivel_vacio &&
                               bytes_de_muestra * 2 <= bytes_completa &&
                               (bases_sin_muestra_.empty() || !bases_sin_muestra_.count(uint32_t(base)));
      const bool muestra_util = con_muestra && textura.intervalo >= 8 && textura.muestra_valida;
      const bool solo_muestra = muestra_util && muestras_comprobadas_ >= kMuestrasAComprobar &&
                                textura.muestras_seguidas + 1u < uint32_t(muestreo_huellas_);
      const uint64_t bytes_huella = solo_muestra ? bytes_de_muestra : bytes_completa + (con_muestra ? bytes_de_muestra : 0);
      // The per-frame budget: textures that doubled their interval together (one zone) are rechecked in the same
      // frame; a stable one that does not fit waits 1-2 frames (kAplazamientoMax times in a row at most). New and
      // changing textures never wait.
      if (fotograma_huellas_ != fotograma_) {
        fotograma_huellas_ = fotograma_;
        bytes_huella_fotograma_ = 0;
      }
      if (presupuesto_huellas_ && textura.con_contenido && textura.intervalo >= 8 && !textura.nivel_vacio &&
          textura.aplazamientos < kAplazamientoMax && bytes_huella_fotograma_ > 0 &&
          bytes_huella_fotograma_ + bytes_huella > presupuesto_huellas_) {
        ++textura.aplazamientos;
        ++huellas_aplazadas_;
        textura.siguiente = fotograma_ + 1 + (clave & 1);
        return listo();
      }
      textura.aplazamientos = 0;
      bytes_huella_fotograma_ += bytes_huella;
      bytes_huella_ += bytes_huella;
      const uint8_t* const crudo_base = memoria_->TranslatePhysical(uint32_t(dir_base));
      const uint8_t* const crudo_mips = extension_mips ? memoria_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
      uint64_t muestra = 0;
      if (con_muestra) {
        muestra = HuellaMuestra(crudo_base, extension, 0);
        if (extension_mips) {
          muestra = HuellaMuestra(crudo_mips, extension_mips, muestra);
        }
      }
      const bool muestra_igual = muestra_util && muestra == textura.huella_muestra;
      if (solo_muestra && muestra_igual) {
        ++textura.muestras_seguidas;
        ++muestras_aciertos_;
        bytes_ahorrados_muestra_ += bytes_completa - bytes_de_muestra;
        return sin_cambios();
      }
      if (solo_muestra) {
        ++muestras_distintas_;  // the sample changed: now the whole hash
        bytes_huella_fotograma_ += bytes_completa;
        bytes_huella_ += bytes_completa;
      }
      uint64_t huella = XXH3_64bits(crudo_base, size_t(extension));
      // fc3_texcruda_bases (DIAGNOSTIC): the guest bytes of the listed bases, once each.
      if (const std::string lista = REXCVAR_GET(fc3_texcruda_bases); !lista.empty()) {
        static std::unordered_map<uint32_t, bool> volcadas;
        static std::vector<uint32_t> pedidas;
        static bool leidas = false;
        if (!leidas) {
          leidas = true;
          size_t i = 0;
          while (i < lista.size()) {
            const size_t j = lista.find(',', i);
            const std::string t = lista.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!t.empty()) pedidas.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 16)) & 0x1FFFF000u);
            if (j == std::string::npos) break;
            i = j + 1;
          }
        }
        const uint32_t b = uint32_t(base) & 0x1FFFF000u;
        // fc3-switch 2026-11-11 DIAGNOSTIC (AO specks): every hash of a listed base (does the AO's 4x4 rotation
        // texture change from frame to frame?), 400 lines at most.
        if (std::find(pedidas.begin(), pedidas.end(), b) != pedidas.end()) {
          static uint32_t lineas_hash = 0;
          if (lineas_hash < 400) {
            ++lineas_hash;
            REXLOG_INFO("[texhash] frame {} {:08X} {}x{} fmt {}: hash {:016X} (previous {:016X}) first bytes {:08X} {:08X}",
                        fotograma_, b, ancho, alto, formato, huella, textura.huella_cruda,
                        extension >= 4 ? *reinterpret_cast<const uint32_t*>(crudo_base) : 0u,
                        extension >= 8 ? *reinterpret_cast<const uint32_t*>(crudo_base + 4) : 0u);
          }
        }
        if (std::find(pedidas.begin(), pedidas.end(), b) != pedidas.end() && !volcadas[b]) {
          volcadas[b] = true;
          for (int parte = 0; parte < 2; ++parte) {
            const uint8_t* datos = parte ? crudo_mips : crudo_base;
            const uint64_t n = parte ? extension_mips : extension;
            if (!datos || !n) continue;
            const std::string ruta =
                fmt::format("sdmc:/switch/fc3-nx/texcruda_{:08X}_{}.bin", b, parte ? "mips" : "base");
            if (FILE* fh = std::fopen(ruta.c_str(), "wb")) {
              std::fwrite(datos, 1, size_t(n), fh);
              std::fclose(fh);
            }
          }
          REXLOG_INFO("[texcruda] frame {} {:08X}: base {} bytes at {:08X}, mips {} bytes at {:08X}, {}x{} fmt {}, fetch "
                      "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                      fotograma_, b, extension, uint32_t(dir_base), extension_mips, uint32_t(dir_mips), ancho, alto,
                      formato, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
      }
      // fc3-switch 2026-11-09 DIAGNOSTIC (black arm): zero share of the guest bytes of 512x1024 DXT1 textures, on
      // every full hash (each change or recheck), 200 lines at most.
      if (formato == 18 && ancho == 512 && alto == 1024) {
        static uint32_t lineas = 0;
        if (lineas < 200) {
          ++lineas;
          uint64_t ceros = 0;
          for (uint64_t i = 0; i < extension; ++i) ceros += crudo_base[i] == 0;
          REXLOG_INFO("[texceros] frame {} {:08X} 512x1024 dxt1: {} bytes, {:.1f}% zero, hash {:016X} (previous {:016X})",
                      fotograma_, base, extension, 100.0 * double(ceros) / double(std::max<uint64_t>(extension, 1)),
                      huella, textura.huella_cruda);
          // and the guest bytes of the first 3 such textures, once each, to the SD (decoded on the PC).
          static uint32_t escritas = 0;
          if (escritas < 3 && fotograma_ > 4500) {
            ++escritas;
            const std::string ruta = fmt::format("sdmc:/switch/fc3-nx/texcruda_{:08X}_f{}_w{}.bin", base, fotograma_,
                                                 f[0]);
            if (FILE* fh = std::fopen(ruta.c_str(), "wb")) {
              std::fwrite(crudo_base, 1, size_t(extension), fh);
              std::fclose(fh);
              REXLOG_INFO("[texceros] wrote {} ({} bytes, fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})", ruta,
                          extension, f[0], f[1], f[2], f[3], f[4], f[5]);
            }
          }
        }
      }
      if (extension_mips) {
        huella = XXH3_64bits_withSeed(crudo_mips, size_t(extension_mips), huella);
      }
      const bool completa_igual = textura.con_contenido && huella == textura.huella_cruda;
      if (muestra_util && !solo_muestra) {
        // The guard: this stable recheck computed both hashes.
        ++muestras_comprobadas_;
        if (muestra_igual && !completa_igual) {
          // A texture the game changes where the sample does not look (runs 9 and 10: the same 1024x1024 DXT1 at
          // 0CB7D000). That address is rechecked whole from now on and the guard starts again for the rest; a
          // fifth such texture switches sampling off for all.
          const bool apagar = bases_sin_muestra_.size() >= kBasesSinMuestraMax;
          REXLOG_WARN("[deko3d] la MUESTRA de la textura {:08X} {}x{} formato {} ({} KB de base y {} KB de mips) no "
                      "vio un cambio que la huella completa si (tras {} recomprobaciones con las dos): {}",
                      base, ancho, alto, formato, extension >> 10, extension_mips >> 10, muestras_comprobadas_,
                      apagar ? "se APAGA el muestreo de huellas"
                             : "esa direccion se comprueba siempre entera y la guardia vuelve a empezar");
          if (apagar) {
            muestreo_huellas_ = 0;
          } else {
            bases_sin_muestra_.insert(uint32_t(base));
            muestras_comprobadas_ = 0;
          }
          textura.muestra_valida = false;
        } else if (muestras_comprobadas_ == kMuestrasAComprobar) {
          REXLOG_INFO("[deko3d] muestreo de huellas: {} recomprobaciones con las dos huellas, ningun desacuerdo; sigue "
                      "con 1 de cada {} completa",
                      muestras_comprobadas_, muestreo_huellas_);
        }
      }
      if (completa_igual) {
        if (con_muestra && muestreo_huellas_ > 0) {
          textura.huella_muestra = muestra;
          textura.muestra_valida = true;
        }
        textura.muestras_seguidas = 0;
        return sin_cambios();
      }
      textura.muestra_valida = false;  // new content: its sample is taken at the next whole check
      textura.muestras_seguidas = 0;
      textura.huella_cruda = huella;
    }
  }
  // Every level, layer after layer, untiled and in host byte order.
  const ImagenDk& img = textura.imagen;
  std::array<size_t, 16> desplazamiento_nivel{};
  std::array<size_t, 16> bytes_capa_nivel{};
  size_t bytes_datos = 0;
  for (uint32_t n = 0; n < img.niveles; ++n) {
    const uint32_t bx_host = (std::max(img.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
    const uint32_t by_host = (std::max(img.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
    desplazamiento_nivel[n] = bytes_datos;
    bytes_capa_nivel[n] = size_t(bx_host) * by_host * tf.bytes;
    bytes_datos += bytes_capa_nivel[n] * capas * (fondo ? fondo : 1);
  }
  temporal_.assign(bytes_datos, 0);
  const uint32_t bloques_x_host = (img.ancho + tf.bloque - 1) / tf.bloque;
  const uint32_t bloques_y_host = (img.alto + tf.bloque - 1) / tf.bloque;
  if (volumen) {
    if (!LeerVolumen(f, tf, bloques_x, bloques_y, fondo, bloques_x_host, bloques_y_host, pitch_bloques, mosaico,
                     temporal_.data())) {
      Avisar(33, "textura fuera de la memoria");
      return textura.con_contenido ? ranura : VaciaDe(tipo_shader);
    }
    // 3D textures have no raw fingerprint: the untiled data decides.
    const uint64_t huella = XXH3_64bits(temporal_.data(), temporal_.size());
    if (textura.con_contenido && huella == textura.huella_cruda) {
      textura.intervalo = REXCVAR_GET(fc3_volumen_cada_fotograma) ? 1 : std::min<uint32_t>(textura.intervalo * 2, 32);
      textura.siguiente = fotograma_ + textura.intervalo;
      return listo();
    }
    textura.huella_cruda = huella;
  } else {
    for (uint32_t c = 0; c < (leer_base ? capas : 0u); ++c) {
      if (!LeerNivel(dir_base + c * zancada_cara, mosaico, pitch_bloques,
                     uint64_t(std::max(pitch_bloques, bloques_x)) * tf.bytes, tf, base_ox, base_oy, bloques_x, bloques_y,
                     bloques_x_host, temporal_.data() + bytes_capa_nivel[0] * c)) {
        Avisar(33, "textura fuera de la memoria");
        return textura.con_contenido ? ranura : VaciaDe(tipo_shader);
      }
    }
    for (uint32_t n = 1; n < img.niveles; ++n) {
      const uint32_t s = nivel_empaquetado == 0 ? 0 : std::min(n, nivel_empaquetado);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= nivel_empaquetado) {
        DesplazamientoEmpaquetado(ancho, alto, tf.bloque, n, ox, oy);
      }
      const uint32_t bx_invitado = (std::max(ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_invitado = (std::max(alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t bx_host = (std::max(img.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_host = (std::max(img.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      bool fuera = false;
      for (uint32_t c = 0; c < capas && !fuera; ++c) {
        fuera = !LeerNivel(dir_mips + regiones[s].desplazamiento + c * regiones[s].zancada, mosaico,
                           regiones[s].pitch_bloques, regiones[s].fila_bytes, tf, ox, oy, std::min(bx_invitado, bx_host),
                           std::min(by_invitado, by_host), bx_host,
                           temporal_.data() + desplazamiento_nivel[n] + bytes_capa_nivel[n] * c);
      }
      if (fuera) {
        Avisar(39, "nivel de mip fuera de la memoria: se queda a cero");
        break;
      }
    }
  }
  // A whole level of zero bytes: not streamed in yet (Gears of War 2's second level drew big structures flat brown
  // from textures uploaded while still empty, and stable textures are only rechecked every 32-39 frames).
  if (!volumen) {
    textura.nivel_vacio = false;
    for (uint32_t n = 0; n < img.niveles && !textura.nivel_vacio; ++n) {
      if (n == 0 && !leer_base) continue;
      const uint8_t* p = temporal_.data() + desplazamiento_nivel[n];
      const size_t b = bytes_capa_nivel[n] * capas;
      textura.nivel_vacio = b >= 64 && std::all_of(p, p + b, [](uint8_t x) { return x == 0; });
    }
    if (textura.nivel_vacio && avisos_nivel_vacio_++ < 32) {
      REXLOG_WARN("[deko3d] textura {:08X} {}x{} formato {}: un nivel entero a cero (aun sin cargar?); se recomprueba "
                  "cada fotograma",
                  base, ancho, alto, formato);
    }
  }
  const uint32_t orden = (f[1] >> 6) & 0x3;
  if (orden && (tf.unidad_orden == 2 || tf.unidad_orden == 4)) {
    CambiarOrdenBytes(temporal_.data(), temporal_.size(), tf.unidad_orden, orden);
  }
  // Diagnostic only: a tiled source range includes padding and may overlap
  // unrelated data. Compare the bytes actually sent to the GPU before
  // proposing any upload-elision optimization. The separately gated experiment
  // may skip a copy only after hashing all bytes including every mip/layer.
  uint64_t huella_desempacada = 0;
  const bool comprobar_desempacada = REXCVAR_GET(fc3_trace_texture_reuploads) ||
                                     REXCVAR_GET(fc3_skip_identical_texture_uploads);
  if (comprobar_desempacada) {
    huella_desempacada = XXH3_64bits(temporal_.data(), temporal_.size());
    static uint64_t comparadas = 0, identicas = 0, bytes_identicos = 0;
    if (textura.con_contenido && textura.huella_desempacada_valida) {
      ++comparadas;
      const bool identica = huella_desempacada == textura.huella_desempacada;
      if (identica) {
        ++identicas;
        bytes_identicos += bytes_datos;
        if (identicas <= 16) {
          REXLOG_INFO("[fc3-reupload] same bytes base={:08X} size={}x{} format={} bytes={}",
                      base, ancho, alto, formato, bytes_datos);
        }
      }
      if ((comparadas & 255) == 0) {
        REXLOG_INFO("[fc3-reupload] compared={} identical={} identical_bytes={}", comparadas, identicas,
                    bytes_identicos);
      }
      if (identica && REXCVAR_GET(fc3_skip_identical_texture_uploads)) {
        // Keep checking each frame: guest memory changed even though the
        // decoded bytes did not. The prior GPU image remains valid.
        textura.intervalo = 1;
        textura.siguiente = fotograma_ + 1;
        return listo();
      }
    }
  }
  // To the upload ring (it may move on to the next slot: the copies and the descriptors go with the slot they were
  // recorded in, and this texture's draws come after).
  const deko::Subida subida = Reservar(uint32_t(bytes_datos), 256);
  if (!subida) {
    Avisar(13, "textura mayor que el anillo de subida");
    return textura.con_contenido ? ranura : VaciaDe(tipo_shader);
  }
  std::memcpy(subida.cpu, temporal_.data(), bytes_datos);
  DkCmdBuf previos = nucleo_.ComandosPrevios();
  const int32_t marca_subida = PerfilCopiaInicio(previos);
  for (uint32_t n = 0; n < img.niveles; ++n) {
    const uint32_t bx_host = (std::max(img.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
    const uint32_t by_host = (std::max(img.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
    DkImageView vista;
    dkImageViewDefaults(&vista, &img.imagen);
    vista.mipLevelOffset = uint8_t(n);
    // Layers (cube faces, 3D slices) by the rectangle's z; rows and layers in bytes of the upload.
    const uint32_t fila = bx_host * tf.bytes;
    const DkCopyBuf origen = {subida.gpu + desplazamiento_nivel[n], fila, fila * by_host};
    const DkImageRect rect = {0, 0, 0, std::max(img.ancho >> n, 1u), std::max(img.alto >> n, 1u),
                              volumen ? fondo : capas};
    dkCmdBufCopyBufferToImage(previos, &origen, &vista, &rect, 0);
  }
  PerfilCopiaFin(previos, marca_subida,
                 {"subida", 0, 0, 0, img.ancho, img.alto, uint32_t(img.formato), uint32_t(bytes_datos)});
  if (comprobar_desempacada) {
    textura.huella_desempacada = huella_desempacada;
    textura.huella_desempacada_valida = true;
  }
  textura.con_contenido = true;
  textura.intervalo = 1;
  textura.siguiente = fotograma_ + 1;
  ++texturas_subidas_;
  bytes_subidos_ += bytes_datos;
  return listo();
}

// Above the limit, textures not checked for kFotogramasSinUsoParaSoltar frames are released until the cache is
// back under 75 % (a texture in use is checked at least every 39 frames). The game streams the city into new
// addresses: without a limit the cache grew ~20 MB every 40 s of racing (docs/native-renderer.md).
void RenderDk::SoltarTexturasSiHaceFalta() {
  const uint64_t limite = uint64_t(REXCVAR_GET(nfsmw_nativo_texturas_mb_max)) << 20;
  if (bytes_texturas_ <= limite) {
    return;
  }
  const uint64_t objetivo = limite / 4 * 3;
  uint64_t soltadas = 0, bytes = 0;
  for (auto it = texturas_.begin(); it != texturas_.end() && bytes_texturas_ > objetivo;) {
    TexturaDk& t = it->second;
    if (t.fotograma != UINT64_MAX && t.fotograma + kFotogramasSinUsoParaSoltar < fotograma_) {
      bytes_texturas_ -= t.bytes;
      bytes += t.bytes;
      ++soltadas;
      DestruirImagen(t.imagen);
      it = texturas_.erase(it);
    } else {
      ++it;
    }
  }
  if (soltadas) {
    ++generacion_texturas_;  // the per-fetch cache may name them
    REXLOG_INFO("[deko3d] texturas: {} soltadas ({} MB); quedan {} ({} MB)", soltadas, bytes >> 20, texturas_.size(),
                bytes_texturas_ >> 20);
  }
}

}  // namespace rex::x360
