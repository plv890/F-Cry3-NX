// rex::deko3d (from nfsmw-nx): provider, presenter and immediate drawer. See rex/ui/deko3d/presentador.h.

#include <rex/ui/deko3d/presentador.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ui/superposicion.h>
#include <rex/ui/surface_switch.h>

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Defined with the input driver, which toggles it with ZL + ZR + Up (switch_input_driver.cpp).
REXCVAR_DECLARE(bool, switch_contador_fps);
// The profiler's stack sampling (switch_perf.cpp), shown in the counter.
extern "C" bool RexSwitchPerfStacksActive(void);

namespace rex::deko3d {
namespace {

using rex::ui::Presenter;

// ---- Shaders of the presenter and the drawer (GLSL compiled by uam at start, on the compile worker)

// A rectangle from gl_VertexID (a triangle strip): its corners in clip space (x0, y0, x1, y1, y up) and the texture
// coordinates that go with them.
constexpr char kVsCopia[] = R"(#version 460
layout(std140, binding = 0) uniform Copia { vec4 destino; vec4 origen; } c;
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 t = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  gl_Position = vec4(mix(c.destino.xy, c.destino.zw, t), 0.0, 1.0);
  v_uv = mix(c.origen.xy, c.origen.zw, t);
}
)";
constexpr char kFsCopia[] = R"(#version 460
layout(binding = 0) uniform sampler2D salida;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(salida, v_uv).rgb, 1.0); }
)";
// Dear ImGui's vertices: position in the drawer's coordinate space (y down), texture coordinate, RGBA8 color.
constexpr char kVsUi[] = R"(#version 460
layout(std140, binding = 0) uniform Proyeccion { vec4 escala_desplazamiento; } p;
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
  gl_Position = vec4(a_pos * p.escala_desplazamiento.xy + p.escala_desplazamiento.zw, 0.0, 1.0);
  v_uv = a_uv;
  v_color = a_color;
}
)";
constexpr char kFsUi[] = R"(#version 460
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, v_uv) * v_color; }
)";

bool CompilarPropio(ProveedorDeko3d& proveedor, Nucleo& nucleo, DkShader& shader, bool vertices, const char* glsl,
                    const char* nombre) {
  Compilador::Resultado r = proveedor.compilador().Compilar(vertices, glsl);
  if (!r.ok) {
    Registro("[dk] el shader propio %s no compila: %s", nombre, r.log.c_str());
    return false;
  }
  return nucleo.CargarShader(shader, r.dksh, nombre);
}

void EstadoSinProfundidad(DkCmdBuf c, bool mezcla) {
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkColorStateSetBlendEnable(&cs, 0, mezcla);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  DkBlendState bs;
  dkBlendStateDefaults(&bs);
  bs.srcColorBlendFactor = DkBlendFactor_SrcAlpha;
  bs.dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
  bs.srcAlphaBlendFactor = DkBlendFactor_One;
  bs.dstAlphaBlendFactor = DkBlendFactor_InvSrcAlpha;
  dkCmdBufBindRasterizerState(c, &rs);
  dkCmdBufBindColorState(c, &cs);
  dkCmdBufBindColorWriteState(c, &cws);
  dkCmdBufBindDepthStencilState(c, &ds);
  dkCmdBufBindBlendState(c, 0, &bs);
}

void Encuadre(DkCmdBuf c, uint32_t ancho, uint32_t alto) {
  const DkViewport vp = {0, 0, float(ancho), float(alto), 0, 1};
  const DkScissor tijera = {0, 0, ancho, alto};
  dkCmdBufSetViewports(c, 0, &vp, 1);
  dkCmdBufSetScissors(c, 0, &tijera, 1);
}

DkSamplerDescriptor DescriptorSampler(DkFilter filtro, DkWrapMode envoltura) {
  DkSampler s;
  dkSamplerDefaults(&s);
  s.minFilter = s.magFilter = filtro;
  s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = envoltura;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  return d;
}

// The UI drawers paint the swapchain image the presenter has just painted the guest output into.
class ContextoUIDeko3d final : public rex::ui::UIDrawContext {
 public:
  ContextoUIDeko3d(Presenter& presentador, uint32_t ancho, uint32_t alto, DkImage& destino)
      : UIDrawContext(presentador, ancho, alto), destino(destino) {}
  DkImage& destino;
};

// ---- The FPS counter (switch_contador_fps)
//
// Painted by the presenter over the guest output, so it needs neither ImGui nor the UI thread (opening the ImGui
// debug overlay moves every paint to the UI thread). Images presented per second and the longest gap between two
// of them in the last half second, in a 5x7 dot font, scaled up with nearest filtering.

constexpr uint32_t kContadorCaracteres = 16;
constexpr uint32_t kContadorAncho = kContadorCaracteres * 6;  // 5 columns and a space per character
constexpr uint32_t kContadorAlto = 9;                         // 7 rows and a margin above and below
constexpr uint32_t kContadorEscala = 3;

// Each glyph: 7 rows, the 5 low bits of each from left (bit 4) to right (bit 0).
const uint8_t* GlifoContador(char c) {
  static const uint8_t kDigitos[10][7] = {
      {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
      {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
      {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
      {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
  static const uint8_t kPunto[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C};
  static const uint8_t kF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
  static const uint8_t kP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
  static const uint8_t kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
  static const uint8_t kR[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
  static const uint8_t kM[7] = {0, 0, 0x1A, 0x15, 0x15, 0x11, 0x11};
  static const uint8_t kSMinuscula[7] = {0, 0, 0x0E, 0x10, 0x0E, 0x01, 0x1E};
  static const uint8_t kEspacio[7] = {};
  if (c >= '0' && c <= '9') return kDigitos[c - '0'];
  switch (c) {
    case '.': return kPunto;
    case 'F': return kF;
    case 'P': return kP;
    case 'S': return kS;
    case 'R': return kR;
    case 'm': return kM;
    case 's': return kSMinuscula;
    default: return kEspacio;
  }
}

// The text as RGBA8 texels: white on black.
void RasterizarContador(const char* texto, uint8_t* destino) {
  std::memset(destino, 0, kContadorAncho * kContadorAlto * 4);
  for (uint32_t i = 0; i < kContadorCaracteres && texto[i]; ++i) {
    const uint8_t* glifo = GlifoContador(texto[i]);
    for (uint32_t fila = 0; fila < 7; ++fila) {
      for (uint32_t columna = 0; columna < 5; ++columna) {
        if (!((glifo[fila] >> (4 - columna)) & 1)) continue;
        uint8_t* t = destino + ((fila + 1) * kContadorAncho + i * 6 + columna) * 4;
        t[0] = t[1] = t[2] = 0xFF;
        t[3] = 0xFF;
      }
    }
  }
}

// ---- The overlay (rex/ui/superposicion.h): a game's options menu, a few lines in the same dot font

constexpr uint32_t kMenuColumnas = 44;
constexpr uint32_t kMenuFilas = 8;
constexpr uint32_t kMenuAncho = (kMenuColumnas + 2) * 6;  // a character of margin each side
constexpr uint32_t kMenuAlto = (kMenuFilas + 1) * 9;      // half a line of margin above and below

struct Superposicion {
  std::mutex candado;
  std::vector<std::string> lineas;
  uint64_t version = 0;
};
Superposicion& LaSuperposicion() {
  static Superposicion s;
  return s;
}

// Upper case, digits and a little punctuation; the counter's glyphs for what they share.
const uint8_t* GlifoMenu(char c) {
  static const uint8_t kLetras[26][7] = {
      {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
      {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
      {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
      {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
      {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
      {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
      {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
      {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
      {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
      {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}};
  static const uint8_t kMayor[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
  static const uint8_t kDosPuntos[7] = {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0};
  static const uint8_t kBarra[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
  static const uint8_t kAbre[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
  static const uint8_t kCierra[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
  static const uint8_t kGuion[7] = {0, 0, 0, 0x1F, 0, 0, 0};
  static const uint8_t kMas[7] = {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
  static const uint8_t kExclamacion[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04};
  static const uint8_t kComa[7] = {0, 0, 0, 0, 0x0C, 0x04, 0x08};
  static const uint8_t kPor[7] = {0, 0, 0x11, 0x0A, 0x04, 0x0A, 0x11};
  if (c >= 'A' && c <= 'Z') return kLetras[c - 'A'];
  switch (c) {
    case '>': return kMayor;
    case ':': return kDosPuntos;
    case '/': return kBarra;
    case '(': return kAbre;
    case ')': return kCierra;
    case '-': return kGuion;
    case '+': return kMas;
    case '!': return kExclamacion;
    case ',': return kComa;
    case 'x': return kPor;
    default: return GlifoContador(c);  // digits, '.', space
  }
}

// The lines as RGBA8 texels: white on black, a character of margin.
void RasterizarMenu(const std::vector<std::string>& lineas, uint8_t* destino) {
  std::memset(destino, 0, kMenuAncho * kMenuAlto * 4);
  for (uint32_t i = 0; i < kMenuAncho * kMenuAlto; ++i) destino[i * 4 + 3] = 0xFF;
  for (uint32_t l = 0; l < kMenuFilas && l < lineas.size(); ++l) {
    for (uint32_t i = 0; i < kMenuColumnas && i < lineas[l].size(); ++i) {
      const uint8_t* glifo = GlifoMenu(lineas[l][i]);
      for (uint32_t fila = 0; fila < 7; ++fila) {
        for (uint32_t columna = 0; columna < 5; ++columna) {
          if (!((glifo[fila] >> (4 - columna)) & 1)) continue;
          uint8_t* t = destino + ((l * 9 + 5 + fila) * kMenuAncho + (i + 1) * 6 + columna) * 4;
          t[0] = t[1] = t[2] = 0xFF;
        }
      }
    }
  }
}

// ---- The presenter

class PresentadorDeko3d final : public Presenter {
 public:
  PresentadorDeko3d(ProveedorDeko3d& proveedor, HostGpuLossCallback perdida)
      : Presenter(perdida), proveedor_(proveedor) {}

  ~PresentadorDeko3d() override {
    ShutdownPaintThread();  // its thread calls PaintAndPresentImpl (presenter.h)
    {
      std::lock_guard<std::mutex> cerrojo(captura_candado_);
      captura_.Destruir();
    }
    cadena_.Destruir();
    proveedor_.dispositivo().EsperarInactiva();
    proveedor_.dispositivo().DestruirBloque(contador_memoria_);
    contador_memoria_ = nullptr;
    proveedor_.dispositivo().DestruirBloque(menu_memoria_);
    menu_memoria_ = nullptr;
    for (ImagenSalida& s : salidas_) {
      proveedor_.dispositivo().DestruirBloque(s.memoria);
      s.memoria = nullptr;
    }
    nucleo_.Destruir();
  }

  bool Inicializar() {
    ConfigNucleo config;
    config.ranuras = 3;
    config.mb_subida = 1;
    config.kb_comandos = 256;
    config.mb_codigo = 1;
    config.mb_trozo_imagenes = 1;
    config.descriptores_imagen = kGuestOutputMailboxSize + 2;  // + the FPS counter's and the overlay's
    config.descriptores_sampler = 2;                          // linear (the output) and nearest (the counter)
    config.kb_consultas = 4;
    nucleo_.Crear(proveedor_.dispositivo(), config);
    if (!CompilarPropio(proveedor_, nucleo_, vs_, true, kVsCopia, "presentador_vs") ||
        !CompilarPropio(proveedor_, nucleo_, fs_, false, kFsCopia, "presentador_fs")) {
      return false;
    }
    // The sampler of the guest output, once
    nucleo_.EmpezarTrabajo(++trabajo_);
    const DkSamplerDescriptor sampler = DescriptorSampler(DkFilter_Linear, DkWrapMode_ClampToEdge);
    dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresSampler(), &sampler, sizeof(sampler));
    const DkSamplerDescriptor cercano = DescriptorSampler(DkFilter_Nearest, DkWrapMode_ClampToEdge);
    dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresSampler() + sizeof(sampler), &cercano, sizeof(cercano));
    // The FPS counter's image and its descriptor, after the guest outputs'.
    {
      Dispositivo& d = proveedor_.dispositivo();
      DkImageLayoutMaker m;
      dkImageLayoutMakerDefaults(&m, d.dispositivo());
      m.format = DkImageFormat_RGBA8_Unorm;
      m.dimensions[0] = kContadorAncho;
      m.dimensions[1] = kContadorAlto;
      DkImageLayout disposicion;
      dkImageLayoutInitialize(&disposicion, &m);
      contador_memoria_ = d.CrearBloque(uint32_t(dkImageLayoutGetSize(&disposicion)),
                                        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "contador de fps");
      dkImageInitialize(&contador_imagen_, &disposicion, contador_memoria_, 0);
      DkImageView v;
      dkImageViewDefaults(&v, &contador_imagen_);
      DkImageDescriptor descriptor;
      dkImageDescriptorInitialize(&descriptor, &v, false, false);
      dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresImagen() + kGuestOutputMailboxSize * sizeof(descriptor),
                       &descriptor, sizeof(descriptor));
    }
    // The overlay's image and its descriptor, after the counter's.
    {
      Dispositivo& d = proveedor_.dispositivo();
      DkImageLayoutMaker m;
      dkImageLayoutMakerDefaults(&m, d.dispositivo());
      m.format = DkImageFormat_RGBA8_Unorm;
      m.dimensions[0] = kMenuAncho;
      m.dimensions[1] = kMenuAlto;
      DkImageLayout disposicion;
      dkImageLayoutInitialize(&disposicion, &m);
      menu_memoria_ = d.CrearBloque(uint32_t(dkImageLayoutGetSize(&disposicion)),
                                    DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "superposicion");
      dkImageInitialize(&menu_imagen_, &disposicion, menu_memoria_, 0);
      DkImageView v;
      dkImageViewDefaults(&v, &menu_imagen_);
      DkImageDescriptor descriptor;
      dkImageDescriptorInitialize(&descriptor, &v, false, false);
      dkCmdBufPushData(nucleo_.comandos(),
                       nucleo_.DescriptoresImagen() + (kGuestOutputMailboxSize + 1) * sizeof(descriptor), &descriptor,
                       sizeof(descriptor));
    }
    nucleo_.TerminarTrabajo();
    return InitializeCommonSurfaceIndependent();
  }

  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override {
    return rex::ui::Surface::kTypeFlag_ViNWindow;
  }

  // The PNG captures (nfsmw_captura_cada_s): the latest guest output, copied to CPU memory with its own command
  // buffer (any thread may call this, Presenter's contract) and turned from RGB10A2 into R8 G8 B8 X8.
  bool CaptureGuestOutput(rex::ui::RawImage& image_out) override {
    std::lock_guard<std::mutex> cerrojo(captura_candado_);
    uint32_t indice;
    GuestOutputProperties propiedades;
    std::unique_lock<std::mutex> consumidor(ConsumeGuestOutput(indice, &propiedades, nullptr));
    if (indice == UINT32_MAX) {
      return false;
    }
    ImagenSalida& s = salidas_[indice];
    const uint32_t ancho = std::min(propiedades.frontbuffer_width, s.ancho);
    const uint32_t alto = std::min(propiedades.frontbuffer_height, s.alto);
    if (!s.memoria || !ancho || !alto) {
      return false;
    }
    const uint32_t bytes = ancho * alto * 4;
    if (!captura_.dispositivo()) {
      ConfigNucleo config;
      config.ranuras = 1;
      config.mb_subida = 1;
      config.kb_comandos = 64;
      config.mb_codigo = 1;
      config.mb_trozo_imagenes = 1;
      config.descriptores_imagen = 1;
      config.descriptores_sampler = 1;
      config.kb_consultas = 4;
      captura_.Crear(proveedor_.dispositivo(), config);
    }
    if (captura_bytes_ < bytes) {
      // A read-back block (CPU-cached, GPU-uncached); the old one goes when the Nucleo does.
      captura_bytes_ = (bytes + 0xFFFF) & ~0xFFFFu;
      captura_bloque_ = captura_.CrearBloqueLectura(captura_bytes_, "captura de la salida");
    }
    dkMemBlockFlushCpuCache(captura_bloque_, 0, bytes);  // before the GPU writes it
    captura_.EmpezarTrabajo(++captura_trabajo_);
    DkCmdBuf c = captura_.comandos();
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);  // what the ring thread wrote into it
    DkImageView v;
    dkImageViewDefaults(&v, &s.imagen);
    const DkImageRect rect = {0, 0, 0, ancho, alto, 1};
    const DkCopyBuf destino = {dkMemBlockGetGpuAddr(captura_bloque_), ancho * 4, ancho * 4 * alto};
    dkCmdBufCopyImageToBuffer(c, &v, &rect, &destino, 0);
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    captura_.TerminarTrabajo();
    captura_.EsperarGpu();
    consumidor.unlock();
    dkMemBlockFlushCpuCache(captura_bloque_, 0, bytes);  // and before the CPU reads it
    const uint32_t* texeles = static_cast<const uint32_t*>(dkMemBlockGetCpuAddr(captura_bloque_));
    image_out.width = ancho;
    image_out.height = alto;
    image_out.stride = size_t(ancho) * 4;
    image_out.data.resize(size_t(bytes));
    uint8_t* salida = image_out.data.data();
    for (uint32_t i = 0; i < ancho * alto; ++i) {
      const uint32_t t = texeles[i];
      salida[i * 4 + 0] = uint8_t(((t & 0x3FF) * 255 + 511) / 1023);
      salida[i * 4 + 1] = uint8_t((((t >> 10) & 0x3FF) * 255 + 511) / 1023);
      salida[i * 4 + 2] = uint8_t((((t >> 20) & 0x3FF) * 255 + 511) / 1023);
      salida[i * 4 + 3] = 0xFF;
    }
    return true;
  }

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      rex::ui::Surface& superficie, uint32_t ancho, uint32_t alto, bool was_paintable,
      bool& is_vsync_implicit_out) override {
    (void)was_paintable;
    is_vsync_implicit_out = false;  // swap interval 0, as the Vulkan path's IMMEDIATE
    if (superficie.GetType() != rex::ui::Surface::kTypeIndex_ViNWindow) {
      return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    NWindow* ventana = static_cast<rex::ui::ViNWindowSurface&>(superficie).window();
    if (cadena_.creada() && ventana == ventana_ && ancho == cadena_.ancho() && alto == cadena_.alto()) {
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    cadena_.Destruir();
    ventana_ = ventana;
    cadena_.Crear(proveedor_.dispositivo(), ancho, alto, 0, ventana);
    REXLOG_INFO("[deko3d] presentador: cadena de {}x{} en la ventana del sistema", ancho, alto);
    return SurfacePaintConnectResult::kSuccess;
  }

  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override { cadena_.Destruir(); }

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t ancho, uint32_t alto,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override {
    ImagenSalida& s = salidas_[mailbox_index];
    if (!s.memoria || s.ancho != ancho || s.alto != alto) {
      Dispositivo& d = proveedor_.dispositivo();
      if (s.memoria) {
        // Another size: the GPU may still be writing or reading the old image
        d.EsperarInactiva();
        d.DestruirBloque(s.memoria);
        s.memoria = nullptr;
      }
      DkImageLayoutMaker m;
      dkImageLayoutMakerDefaults(&m, d.dispositivo());
      m.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
      m.format = kFormatoSalida;
      m.dimensions[0] = ancho;
      m.dimensions[1] = alto;
      ArreglarTamanoDeTesela(m, alto);
      DkImageLayout disposicion;
      dkImageLayoutInitialize(&disposicion, &m);
      char nombre[48];
      std::snprintf(nombre, sizeof(nombre), "salida del juego %u (%ux%u)", mailbox_index, ancho, alto);
      s.memoria = d.CrearBloque(uint32_t(dkImageLayoutGetSize(&disposicion)),
                                DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, nombre);
      dkImageInitialize(&s.imagen, &disposicion, s.memoria, 0);
      s.ancho = ancho;
      s.alto = alto;
      s.version = siguiente_version_++;
    }
    ContextoSalidaDeko3d contexto(is_8bpc_out_ref, s.imagen, ancho, alto, s.version);
    return refresher(contexto);
  }

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override {
    if (!cadena_.creada()) {
      return PaintResult::kNotPresentedConnectionOutdated;
    }
    const int imagen = cadena_.Adquirir();
    const uint32_t ancho = cadena_.ancho(), alto = cadena_.alto();
    nucleo_.EmpezarTrabajo(++trabajo_);
    DkCmdBuf c = nucleo_.comandos();
    nucleo_.BindDescriptores();
    nucleo_.InvalidarCodigoSiHaceFalta();
    const bool contador = REXCVAR_GET(switch_contador_fps) && contador_memoria_;
    if (contador) {
      ActualizarContador(c);
    }
    const bool menu = menu_memoria_ && ActualizarMenu(c);
    DkImageView vista;
    dkImageViewDefaults(&vista, &cadena_.Imagen(imagen));
    dkCmdBufBindRenderTarget(c, &vista, nullptr);
    Encuadre(c, ancho, alto);
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0, 0, 0, 1);  // the letterbox

    GuestOutputProperties propiedades;
    GuestOutputPaintConfig configuracion;
    uint32_t indice;
    {
      std::unique_lock<std::mutex> consumidor(ConsumeGuestOutput(indice, &propiedades, &configuracion));
    }
    // The mailbox does not give the refresher this image again until a newer one has been acquired, and an image
    // is only destroyed (a new size) after the GPU is idle: it can be used after leaving the lock.
    if (indice != UINT32_MAX) {
      ImagenSalida& s = salidas_[indice];
      const GuestOutputPaintFlow flujo =
          GetGuestOutputPaintFlow(propiedades, ancho, alto, 16384, 16384, configuracion);
      if (flujo.effect_count && s.memoria) {
        uint32_t invalidar = DkInvalidateFlags_Image;
        if (s.version_descriptor != s.version) {
          DkImageView v;
          dkImageViewDefaults(&v, &s.imagen);
          DkImageDescriptor d;
          dkImageDescriptorInitialize(&d, &v, false, false);
          dkCmdBufPushData(c, nucleo_.DescriptoresImagen() + indice * sizeof(d), &d, sizeof(d));
          s.version_descriptor = s.version;
          invalidar |= DkInvalidateFlags_Descriptors;
        }
        // The ring thread's writes into the image end before it is sampled (the GPU overlaps submissions)
        dkCmdBufBarrier(c, DkBarrier_Fragments, invalidar);
        const auto [salida_ancho, salida_alto] = flujo.effect_output_sizes[flujo.effect_count - 1];
        const float x0 = float(flujo.output_x) / float(ancho) * 2 - 1;
        const float x1 = float(flujo.output_x + int32_t(salida_ancho)) / float(ancho) * 2 - 1;
        const float y0 = 1 - float(flujo.output_y) / float(alto) * 2;
        const float y1 = 1 - float(flujo.output_y + int32_t(salida_alto)) / float(alto) * 2;
        const float u1 = float(propiedades.frontbuffer_width) / float(s.ancho);
        const float v1 = float(propiedades.frontbuffer_height) / float(s.alto);
        const float datos[8] = {x0, y0, x1, y1, 0, 0, u1, v1};
        const Subida ubo = nucleo_.Reservar(sizeof(datos), DK_UNIFORM_BUF_ALIGNMENT);
        if (ubo) {
          std::memcpy(ubo.cpu, datos, sizeof(datos));
          EstadoSinProfundidad(c, false);
          dkCmdBufBindVtxAttribState(c, nullptr, 0);
          dkCmdBufBindVtxBufferState(c, nullptr, 0);
          const DkShader* par[] = {&vs_, &fs_};
          dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, par, 2);
          dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, ubo.gpu, sizeof(datos));
          const DkResHandle h = dkMakeTextureHandle(indice, 0);
          dkCmdBufBindTextures(c, DkStage_Fragment, 0, &h, 1);
          dkCmdBufDraw(c, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        }
      }
    }
    if (contador) {
      PintarContador(c, ancho, alto);
    }
    if (menu) {
      PintarMenu(c, ancho, alto);
    }
    nucleo_.TerminarTrabajo();

    if (execute_ui_drawers) {
      ContextoUIDeko3d contexto(*this, ancho, alto, cadena_.Imagen(imagen));
      ExecuteUIDrawersFromUIThread(contexto);
    }
    cadena_.Presentar(imagen);
    return PaintResult::kPresented;
  }

 private:
  // Counts this image and, twice a second, writes the new text into the counter's image.
  void ActualizarContador(DkCmdBuf c) {
    const auto ahora = std::chrono::steady_clock::now();
    if (contador_ultimo_ != std::chrono::steady_clock::time_point{}) {
      contador_hueco_max_ = std::max(contador_hueco_max_, std::chrono::duration<double, std::milli>(ahora - contador_ultimo_).count());
    }
    contador_ultimo_ = ahora;
    ++contador_imagenes_;
    if (contador_desde_ == std::chrono::steady_clock::time_point{}) {
      contador_desde_ = ahora;
    }
    const double segundos = std::chrono::duration<double>(ahora - contador_desde_).count();
    if (segundos < 0.5 && contador_escrito_) {
      return;
    }
    char texto[kContadorCaracteres + 1];
    const double fps = segundos > 0 ? double(contador_imagenes_ - 1) / segundos : 0.0;
    // PRF instead of FPS while the profiler samples stacks (ZL+ZR+R): the only sign of it on screen.
    std::snprintf(texto, sizeof(texto), "%4.1f %s %4.1fms", std::min(fps, 999.9),
                  RexSwitchPerfStacksActive() ? "PRF" : "FPS", std::min(contador_hueco_max_, 999.9));
    contador_desde_ = ahora;
    contador_imagenes_ = 1;
    contador_hueco_max_ = 0;
    const Subida subida = nucleo_.Reservar(kContadorAncho * kContadorAlto * 4, 256);
    if (!subida) {
      return;
    }
    RasterizarContador(texto, subida.cpu);
    DkImageView v;
    dkImageViewDefaults(&v, &contador_imagen_);
    const DkCopyBuf origen = {subida.gpu, 0, 0};
    const DkImageRect rect = {0, 0, 0, kContadorAncho, kContadorAlto, 1};
    dkCmdBufCopyBufferToImage(c, &origen, &v, &rect, 0);
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);
    contador_escrito_ = true;
  }

  // The counter at the top left, scaled up, over whatever was painted.
  void PintarContador(DkCmdBuf c, uint32_t ancho, uint32_t alto) {
    if (!contador_escrito_) return;
    const float x0 = float(16) / float(ancho) * 2 - 1;
    const float y0 = 1 - float(16) / float(alto) * 2;
    const float x1 = x0 + float(kContadorAncho * kContadorEscala) / float(ancho) * 2;
    const float y1 = y0 - float(kContadorAlto * kContadorEscala) / float(alto) * 2;
    const float datos[8] = {x0, y0, x1, y1, 0, 0, 1, 1};
    const Subida ubo = nucleo_.Reservar(sizeof(datos), DK_UNIFORM_BUF_ALIGNMENT);
    if (!ubo) return;
    std::memcpy(ubo.cpu, datos, sizeof(datos));
    EstadoSinProfundidad(c, false);
    dkCmdBufBindVtxAttribState(c, nullptr, 0);
    dkCmdBufBindVtxBufferState(c, nullptr, 0);
    const DkShader* par[] = {&vs_, &fs_};
    dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, par, 2);
    dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, ubo.gpu, sizeof(datos));
    const DkResHandle h = dkMakeTextureHandle(kGuestOutputMailboxSize, 1);  // its image, the nearest sampler
    dkCmdBufBindTextures(c, DkStage_Fragment, 0, &h, 1);
    dkCmdBufDraw(c, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
  }

  // The overlay: new text into its image when it changed; false when there is none to paint.
  bool ActualizarMenu(DkCmdBuf c) {
    Superposicion& s = LaSuperposicion();
    std::vector<std::string> lineas;
    {
      std::lock_guard<std::mutex> cerrojo(s.candado);
      if (s.lineas.empty()) return false;
      if (s.version == menu_version_) return true;
      lineas = s.lineas;
      menu_version_ = s.version;
    }
    const Subida subida = nucleo_.Reservar(kMenuAncho * kMenuAlto * 4, 256);
    if (!subida) return true;
    RasterizarMenu(lineas, subida.cpu);
    DkImageView v;
    dkImageViewDefaults(&v, &menu_imagen_);
    const DkCopyBuf origen = {subida.gpu, 0, 0};
    const DkImageRect rect = {0, 0, 0, kMenuAncho, kMenuAlto, 1};
    dkCmdBufCopyBufferToImage(c, &origen, &v, &rect, 0);
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);
    return true;
  }

  // The overlay in the middle of the screen, scaled up by whole steps.
  void PintarMenu(DkCmdBuf c, uint32_t ancho, uint32_t alto) {
    const uint32_t escala = std::max<uint32_t>(2, std::min(ancho * 9 / 10 / kMenuAncho, alto / 3 / kMenuAlto));
    const float w = float(kMenuAncho * escala) / float(ancho) * 2, h = float(kMenuAlto * escala) / float(alto) * 2;
    const float x0 = -w / 2, y0 = h / 2;
    const float datos[8] = {x0, y0, x0 + w, y0 - h, 0, 0, 1, 1};
    const Subida ubo = nucleo_.Reservar(sizeof(datos), DK_UNIFORM_BUF_ALIGNMENT);
    if (!ubo) return;
    std::memcpy(ubo.cpu, datos, sizeof(datos));
    EstadoSinProfundidad(c, false);
    dkCmdBufBindVtxAttribState(c, nullptr, 0);
    dkCmdBufBindVtxBufferState(c, nullptr, 0);
    const DkShader* par[] = {&vs_, &fs_};
    dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, par, 2);
    dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, ubo.gpu, sizeof(datos));
    const DkResHandle h2 = dkMakeTextureHandle(kGuestOutputMailboxSize + 1, 1);  // its image, the nearest sampler
    dkCmdBufBindTextures(c, DkStage_Fragment, 0, &h2, 1);
    dkCmdBufDraw(c, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
  }

  DkMemBlock menu_memoria_ = nullptr;
  DkImage menu_imagen_{};
  uint64_t menu_version_ = 0;

  DkMemBlock contador_memoria_ = nullptr;
  DkImage contador_imagen_{};
  bool contador_escrito_ = false;
  uint64_t contador_imagenes_ = 0;
  double contador_hueco_max_ = 0;
  std::chrono::steady_clock::time_point contador_desde_{}, contador_ultimo_{};

  struct ImagenSalida {
    DkMemBlock memoria = nullptr;
    DkImage imagen{};
    uint32_t ancho = 0, alto = 0;
    uint64_t version = 0;
    uint64_t version_descriptor = 0;  // the image whose descriptor is in the table (paint thread only)
  };

  ProveedorDeko3d& proveedor_;
  Nucleo nucleo_;  // the painting thread's
  Presentacion cadena_;
  NWindow* ventana_ = nullptr;
  DkShader vs_{}, fs_{};
  uint64_t trabajo_ = 0;
  std::array<ImagenSalida, kGuestOutputMailboxSize> salidas_{};
  uint64_t siguiente_version_ = 1;
  // CaptureGuestOutput's own recorder and read-back memory
  std::mutex captura_candado_;
  Nucleo captura_;
  DkMemBlock captura_bloque_ = nullptr;
  uint32_t captura_bytes_ = 0;
  uint64_t captura_trabajo_ = 0;
};

// ---- Dear ImGui

class DibujanteDeko3d;

class TexturaDeko3d final : public rex::ui::ImmediateTexture {
 public:
  TexturaDeko3d(DibujanteDeko3d& dueno, uint32_t ancho, uint32_t alto)
      : ImmediateTexture(ancho, alto), dueno(dueno) {}
  ~TexturaDeko3d() override;
  DibujanteDeko3d& dueno;
  uint32_t ranura = 0;   // image descriptor
  uint32_t sampler = 0;  // sampler descriptor
  BloqueImagen memoria;
  DkImage imagen{};
};

class DibujanteDeko3d final : public rex::ui::ImmediateDrawer {
 public:
  // Samplers 0-3: nearest/linear x clamp/repeat. Image descriptor 0: a white texel for draws without a texture.
  static constexpr uint32_t kTexturas = 256;

  explicit DibujanteDeko3d(ProveedorDeko3d& proveedor) : proveedor_(proveedor) {}

  ~DibujanteDeko3d() override { nucleo_.Destruir(); }

  bool Inicializar() {
    ConfigNucleo config;
    config.ranuras = 3;
    config.mb_subida = 4;
    config.kb_comandos = 256;
    config.mb_codigo = 1;
    config.mb_trozo_imagenes = 8;
    config.descriptores_imagen = kTexturas;
    config.descriptores_sampler = 4;
    config.kb_consultas = 4;
    nucleo_.Crear(proveedor_.dispositivo(), config);
    if (!CompilarPropio(proveedor_, nucleo_, vs_, true, kVsUi, "ui_vs") ||
        !CompilarPropio(proveedor_, nucleo_, fs_, false, kFsUi, "ui_fs")) {
      return false;
    }
    for (uint32_t i = kTexturas; i-- > 1;) {
      libres_.push_back(i);
    }
    nucleo_.EmpezarTrabajo(++trabajo_);
    const DkSamplerDescriptor samplers[4] = {
        DescriptorSampler(DkFilter_Nearest, DkWrapMode_ClampToEdge),
        DescriptorSampler(DkFilter_Linear, DkWrapMode_ClampToEdge),
        DescriptorSampler(DkFilter_Nearest, DkWrapMode_Repeat),
        DescriptorSampler(DkFilter_Linear, DkWrapMode_Repeat)};
    dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresSampler(), samplers, sizeof(samplers));
    const uint32_t blanco = 0xFFFFFFFFu;
    Subir(blanco_, 0, 1, 1, reinterpret_cast<const uint8_t*>(&blanco));
    nucleo_.TerminarTrabajo();
    return true;
  }

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t ancho, uint32_t alto,
                                                           rex::ui::ImmediateTextureFilter filtro, bool repetida,
                                                           const uint8_t* datos) override {
    if (libres_.empty() || !ancho || !alto) {
      REXLOG_ERROR("[deko3d] dibujante: no quedan descriptores para una textura de {}x{}", ancho, alto);
      return nullptr;
    }
    auto t = std::make_unique<TexturaDeko3d>(*this, ancho, alto);
    t->ranura = libres_.back();
    libres_.pop_back();
    t->sampler = (filtro == rex::ui::ImmediateTextureFilter::kLinear ? 1 : 0) + (repetida ? 2 : 0);
    // In the middle of a frame (ImGui uploads its font on the first draw) the copy goes into that frame's
    // commands, before its draws; otherwise in a work of its own.
    const bool propia = !nucleo_.TrabajoAbierto();
    if (propia) {
      nucleo_.EmpezarTrabajo(++trabajo_);
    }
    Subir(*t, t->ranura, ancho, alto, datos);
    if (propia) {
      nucleo_.TerminarTrabajo();
    }
    return t;
  }

  TexturaDeko3d& blanco() { return blanco_; }

  void Liberar(TexturaDeko3d& t) {
    nucleo_.LiberarImagenLuego(t.memoria);
    libres_.push_back(t.ranura);  // reused by a later texture: its descriptor is written in a later work
  }

  void Begin(rex::ui::UIDrawContext& contexto, float ancho_espacio, float alto_espacio) override {
    ImmediateDrawer::Begin(contexto, ancho_espacio, alto_espacio);
    auto& c = static_cast<ContextoUIDeko3d&>(contexto);
    nucleo_.EmpezarTrabajo(++trabajo_);
    DkCmdBuf cmd = nucleo_.comandos();
    nucleo_.BindDescriptores();
    nucleo_.InvalidarCodigoSiHaceFalta();
    DkImageView vista;
    dkImageViewDefaults(&vista, &c.destino);
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
    Encuadre(cmd, c.render_target_width(), c.render_target_height());
    EstadoSinProfundidad(cmd, true);
    const DkShader* par[] = {&vs_, &fs_};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, par, 2);
    const DkVtxAttribState atributos[3] = {
        {0, 0, offsetof(rex::ui::ImmediateVertex, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(rex::ui::ImmediateVertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(rex::ui::ImmediateVertex, color), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0}};
    const DkVtxBufferState bufer = {sizeof(rex::ui::ImmediateVertex), 0};
    dkCmdBufBindVtxAttribState(cmd, atributos, 3);
    dkCmdBufBindVtxBufferState(cmd, &bufer, 1);
    // From the coordinate space (y down) to clip space (y up)
    const float proyeccion[4] = {2.0f / ancho_espacio, -2.0f / alto_espacio, -1.0f, 1.0f};
    const Subida ubo = nucleo_.Reservar(sizeof(proyeccion), DK_UNIFORM_BUF_ALIGNMENT);
    if (ubo) {
      std::memcpy(ubo.cpu, proyeccion, sizeof(proyeccion));
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, ubo.gpu, sizeof(proyeccion));
    }
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& lote) override {
    lote_valido_ = false;
    con_indices_ = lote.indices && lote.index_count > 0;
    const uint32_t bytes_v = uint32_t(lote.vertex_count) * sizeof(rex::ui::ImmediateVertex);
    const uint32_t bytes_i = con_indices_ ? uint32_t(lote.index_count) * sizeof(uint16_t) : 0;
    const Subida v = nucleo_.Reservar(bytes_v, 16);
    const Subida i = bytes_i ? nucleo_.Reservar(bytes_i, 16) : Subida{};
    if (!v || (bytes_i && !i)) {
      return;
    }
    std::memcpy(v.cpu, lote.vertices, bytes_v);
    dkCmdBufBindVtxBuffer(nucleo_.comandos(), 0, v.gpu, bytes_v);
    if (bytes_i) {
      std::memcpy(i.cpu, lote.indices, bytes_i);
      dkCmdBufBindIdxBuffer(nucleo_.comandos(), DkIdxFormat_Uint16, i.gpu);
    }
    lote_valido_ = true;
  }

  void Draw(const rex::ui::ImmediateDraw& dibujo) override {
    if (!lote_valido_ || dibujo.count <= 0) {
      return;
    }
    uint32_t x, y, w, h;
    if (!ScissorToRenderTarget(dibujo, x, y, w, h)) {
      return;
    }
    DkCmdBuf cmd = nucleo_.comandos();
    const DkScissor tijera = {x, y, w, h};
    dkCmdBufSetScissors(cmd, 0, &tijera, 1);
    const auto* t = static_cast<const TexturaDeko3d*>(dibujo.texture);
    const DkResHandle handle = t ? dkMakeTextureHandle(t->ranura, t->sampler) : dkMakeTextureHandle(0, 0);
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &handle, 1);
    const DkPrimitive primitiva =
        dibujo.primitive_type == rex::ui::ImmediatePrimitiveType::kLines ? DkPrimitive_Lines : DkPrimitive_Triangles;
    if (con_indices_) {
      dkCmdBufDrawIndexed(cmd, primitiva, uint32_t(dibujo.count), 1, uint32_t(dibujo.index_offset),
                          dibujo.base_vertex, 0);
    } else {
      dkCmdBufDraw(cmd, primitiva, uint32_t(dibujo.count), 1, uint32_t(dibujo.base_vertex), 0);
    }
  }

  void EndDrawBatch() override { lote_valido_ = false; }

  void End() override {
    nucleo_.TerminarTrabajo();
    ImmediateDrawer::End();
  }

 private:
  // Records the upload of RGBA8 texels and the image's descriptor into the open work.
  void Subir(TexturaDeko3d& t, uint32_t ranura, uint32_t ancho, uint32_t alto, const uint8_t* datos) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, nucleo_.dispositivo());
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = ancho;
    m.dimensions[1] = alto;
    ArreglarTamanoDeTesela(m, alto);
    DkImageLayout disposicion;
    dkImageLayoutInitialize(&disposicion, &m);
    t.memoria = nucleo_.ReservarImagen(uint32_t(dkImageLayoutGetSize(&disposicion)),
                                       dkImageLayoutGetAlignment(&disposicion));
    dkImageInitialize(&t.imagen, &disposicion, t.memoria.bloque, t.memoria.offset);
    DkCmdBuf cmd = nucleo_.comandos();
    const uint32_t bytes = ancho * alto * 4;
    const Subida s = nucleo_.Reservar(bytes, 256);
    if (s && datos) {
      std::memcpy(s.cpu, datos, bytes);
      DkImageView vista;
      dkImageViewDefaults(&vista, &t.imagen);
      const DkCopyBuf origen = {s.gpu, 0, 0};
      const DkImageRect rect = {0, 0, 0, ancho, alto, 1};
      dkCmdBufCopyBufferToImage(cmd, &origen, &vista, &rect, 0);
    }
    DkImageView vista;
    dkImageViewDefaults(&vista, &t.imagen);
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &vista, false, false);
    dkCmdBufPushData(cmd, nucleo_.DescriptoresImagen() + ranura * sizeof(d), &d, sizeof(d));
    // the copy (copy engine) and the descriptor before any draw samples it
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  }

  ProveedorDeko3d& proveedor_;
  Nucleo nucleo_;  // the UI thread's
  DkShader vs_{}, fs_{};
  TexturaDeko3d blanco_{*this, 1, 1};
  std::vector<uint32_t> libres_;
  uint64_t trabajo_ = 0;
  bool lote_valido_ = false;
  bool con_indices_ = false;
};

TexturaDeko3d::~TexturaDeko3d() {
  if (this != &dueno.blanco()) {
    dueno.Liberar(*this);
  }
}

}  // namespace

// ---- The provider

std::unique_ptr<ProveedorDeko3d> ProveedorDeko3d::Crear() {
  // The core's lines (device, memory blocks, uam, errors) into the game's log, logs/nfsmw_NNN.log next to the NRO.
  // A deko3d failure ends the process: the core flushes before every creation and before Fatal.
  FijarRegistro([](const char* linea) { REXLOG_INFO("{}", linea); }, [] { rex::FlushLogging(); });
  std::unique_ptr<ProveedorDeko3d> p(new ProveedorDeko3d());
  p->dispositivo_.Crear(true);
  p->compilador_ = std::make_unique<Compilador>();
  return p;
}

ProveedorDeko3d::~ProveedorDeko3d() {
  compilador_.reset();
  dispositivo_.Destruir();
}

std::unique_ptr<rex::ui::Presenter> ProveedorDeko3d::CreatePresenter(
    rex::ui::Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  auto p = std::make_unique<PresentadorDeko3d>(*this, host_gpu_loss_callback);
  if (!p->Inicializar()) {
    REXLOG_ERROR("[deko3d] No se pudo inicializar el presentador");
    return nullptr;
  }
  return p;
}

std::unique_ptr<rex::ui::ImmediateDrawer> ProveedorDeko3d::CreateImmediateDrawer() {
  auto d = std::make_unique<DibujanteDeko3d>(*this);
  if (!d->Inicializar()) {
    REXLOG_ERROR("[deko3d] No se pudo inicializar el dibujante de la interfaz");
    return nullptr;
  }
  return d;
}

}  // namespace rex::deko3d

namespace rex::ui {
void FijarSuperposicion(std::vector<std::string> lineas) {
  auto& s = rex::deko3d::LaSuperposicion();
  std::lock_guard<std::mutex> cerrojo(s.candado);
  if (lineas == s.lineas) return;
  s.lineas = std::move(lineas);
  ++s.version;
}
}  // namespace rex::ui
