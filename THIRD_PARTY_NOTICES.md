# Third-party notices

This unofficial Far Cry 3 Switch port builds on the projects listed below. Their
licenses apply to their respective code and derived portions, not to every file
or to the original game. Copyright and attribution remain with the respective
authors. Inherited portions credited to ChanseyIsTheBest retain that attribution
(copyright (c) 2026 ChanseyIsTheBest); this is not a claim that all new FC3 work
was written by that author.

The root `LICENSE` contains GPL-3.0. ReXGlue SDK portions carry BSD-3-Clause
terms, XenosRecomp portions carry MIT terms, and other dependencies retain their
own licenses and per-file exceptions. These notices do not relicense any code.

## Source snapshot and release limitations

The existing source snapshot is **not the exact source tree used to build the
packaged preview binary**. Historical patches and dependency-fetch scripts are
useful provenance, but are not a complete record of all changes or proof that
the binary can be rebuilt or relinked from this snapshot. No exact-rebuild or
complete corresponding-source claim is made here.

The release `licenses/` directory contains the license texts identified below.
Most were copied verbatim from the local dependency checkout or installed
Switch portlibs; two files contain only source-header notices. The libnx,
tiny-AES-c and Disruptor++ texts were retrieved from their public upstream
repositories because their license files were absent from the inspected local
install/checkouts. Those upstream texts do not establish the exact revisions
linked into the binary.

This collection is not a guarantee of legal compliance. Matching source,
modifications, build/relink materials, toolchain-runtime terms and additional
per-file notices must be reconciled separately before making a complete
source or redistribution-compliance claim.

## Projects and inherited work

| Project / authors | Contribution or provenance | License / notice |
|---|---|---|
| [fc3-nx](https://github.com/barbaruiva25/fc3-nx) by barbaruiva25 | Direct upstream FC3 port and application adaptation | Root GPL-3.0; component licenses remain applicable |
| [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty | Inherited application structure, native-renderer work, shader-library tools, CPU enhancement framework and scripts | GPL-3.0 |
| Inherited port work by ChanseyIsTheBest | Portions inherited through the upstream port tree, under their original notices | GPL-3.0 or the applicable component license; not blanket authorship of this FC3 port |
| [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND | Horizon SDK layer, inherited NVK integration, profiling and PGO methods | GPL-3.0; BSD-3-Clause for SDK portions |
| [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip | Earlier application hooks, configuration, code-generator setup, build and analysis scripts | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) by Ben Vanik and contributors | Code generator, Xbox 360 kernel/runtime, command processing, audio and virtual filesystem; the inherited base is v0.10.0, with later local changes | BSD-3-Clause; `ReXGlue-SDK-BSD-3-Clause.txt` |
| Horizon SDK layer by StevensND | Guest memory, exception handling, threading, input, audio, presentation and Switch integration | BSD-3-Clause as part of the SDK |
| Scripts and toolchain integration by StevensND | Inherited `find_gaps.py`, `read_before_write.py`, `direct_calls.py`, `fetch_thirdparty.py` and Switch toolchain work | GPL-3.0 |
| [nx-mod/rexglue-sdk-nx](https://github.com/nx-mod/rexglue-sdk-nx) and ds1-nx contributors | deko3d backend and game-layer integration identified in the local FC3 build files | SDK and application portions retain their respective notices; do not conflate them with deko3d's own license |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors | Xenos shader translator, with inherited nfsmw-nx, masseffect-nx and local changes | MIT; `XenosRecomp-MIT.txt`, copied from `shaders/LICENSE.md` |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico, with NaGaa95's updates, on top of [Mesa](https://mesa3d.org) | Inherited NVK/NAK graphics integration; the local deko3d build configuration still retains an NVK/Vulkan link | Mesa's per-file licenses, predominantly MIT; not a claim that this preview uses an unmodified upstream driver |

## Runtime and graphics dependencies

Filenames in this table are relative to the release archive's `licenses/`
directory, not a promise that the source snapshot vendors every dependency.
The tables describe known dependencies and retained build paths; they are not
an exact linked-object inventory of the preview NRO.

| Component | Use / authors | License text supplied |
|---|---|---|
| [deko3d](https://github.com/devkitPro/deko3d) | Switch graphics API, fincs and contributors | zlib; `deko3d-zlib.txt` |
| [UAM](https://github.com/devkitPro/uam) | Shader compiler, fincs; includes Mesa-derived compiler code | zlib and Mesa MIT terms; `UAM-license.txt` (also directs readers to per-file notices) |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | SPIR-V translation in the deko3d path, Khronos and contributors | Apache-2.0; `SPIRV-Cross-Apache-2.0.txt` |
| [FFmpeg](https://ffmpeg.org) | libavcodec/libavutil audio and decoder support, FFmpeg contributors | LGPL-2.1-or-later for the selected LGPL configuration; `FFmpeg-LGPL-2.1.txt`, `FFmpeg-license-overview.txt` |
| [libmspack](https://www.cabextract.org.uk/libmspack/) | LZX decompression, Stuart Caie; LZX by Jonathan Forbes and Tomi Poutanen, adapted by Microsoft | LGPL-2.1; `libmspack-LGPL-2.1.txt` |
| [fmt](https://github.com/fmtlib/fmt) | Formatting, Victor Zverovich and contributors | MIT with the upstream optional exception; `fmt-license.txt` |
| [spdlog](https://github.com/gabime/spdlog) | Logging, Gabi Melman and contributors | MIT; `spdlog-MIT.txt` |
| [xxHash](https://github.com/Cyan4973/xxHash) | Hashing, Yann Collet | BSD-2-Clause; `xxHash-BSD-2-Clause.txt` |
| [SIMDe](https://github.com/simd-everywhere/simde) | SIMD portability, Evan Nemerson and contributors | MIT; `SIMDe-MIT.txt` |
| [toml++](https://github.com/marzer/tomlplusplus) | Configuration parsing, Mark Gillard | MIT; `tomlplusplus-MIT.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) | UI support, Omar Cornut and contributors | MIT; `Dear-ImGui-MIT.txt` |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | Runtime heap allocator, Pavel Kirienko | MIT; `o1heap-MIT.txt` |
| [utfcpp](https://github.com/nemtrif/utfcpp) | UTF-8 handling, Nemanja Trifunovic and contributors | BSL-1.0; `utfcpp-BSL-1.0.txt` |
| [aes_128](https://github.com/openluopworld/aes_128) | Kernel cryptography, LuoPeng | MIT; `aes_128-MIT.txt` |
| [tiny-AES-c](https://github.com/kokke/tiny-AES-c) | AES/XEX support, kokke and contributors | Unlicense; `tiny-AES-c-Unlicense.txt` |
| [Disruptor++](https://github.com/lewissbaker/disruptorplus) | Queue primitives, Lewis Baker | MIT; `disruptorplus-MIT.txt` |
| [glslang](https://github.com/KhronosGroup/glslang) | Shader translation in retained graphics paths | Multiple BSD, MIT, Apache and other terms; `glslang-licenses.txt` |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | SPIR-V processing | Apache-2.0; `SPIRV-Tools-Apache-2.0.txt` |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | SPIR-V declarations | Khronos MIT-style terms and other file-specific terms; `SPIRV-Headers-licenses.txt` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Vulkan declarations | Apache-2.0 and MIT as specified per file; `Vulkan-Headers-license-overview.txt`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Retained Vulkan support | MIT; `Vulkan-Memory-Allocator-MIT.txt` |
| [libnx](https://github.com/switchbrew/libnx) | Switch system runtime, libnx authors | ISC; `libnx-ISC.txt` |
| [Zstandard](https://github.com/facebook/zstd) | devkitPro Switch compression library, Meta Platforms and contributors | BSD-3-Clause / GPL-2.0 alternatives; both installed texts retained as `Zstandard-BSD-3-Clause.txt` and `Zstandard-GPL-2.0.txt` |
| [zlib](https://zlib.net) | devkitPro Switch compression library | zlib; `zlib-license.txt` |

### Other inherited SDK, host and toolchain components

- [CLI11](https://github.com/CLIUtils/CLI11): BSD-3-Clause,
  `CLI11-BSD-3-Clause.txt`; command-line tooling by Henry Schreiner and
  contributors, University of Cincinnati.
- PowerPC disassembler (`disasm`): GNU binutils/GDB-derived portions, Free
  Software Foundation, Ian Lance Taylor and Cygnus Support. The original
  GPL-2.0-or-later and GPL-1.0-or-later header notices are retained in
  `PowerPC-disasm-notices.txt`, without implementation code. A full GPL v2
  text is also present in `Zstandard-GPL-2.0.txt`; the root GPL v3 text is
  supplied separately. These notices are not covered by the SDK's BSD label.
- RenderDoc API header by Baldur Karlsson: MIT; `RenderDoc-API-MIT.txt` is
  the original license comment only, not the API implementation.
- SDL3 by Sam Lantinga and contributors: zlib; `SDL3-zlib.txt`. SDL3 is used
  by computer builds; the inspected Switch configuration uses libnx backends.
- MoltenVK and the Vulkan loader (Apache-2.0), and Catch2 (BSL-1.0), remain
  acknowledged as inherited host/test dependencies, not Switch runtime claims.
- DirectXShaderCompiler, LLVM and Emscripten are acknowledged for inherited
  host/WebAssembly shader-tool workflows, under their applicable upstream
  University of Illinois/NCSA, MIT, LLVM and component terms. This release
  does not promise or describe a separate web installer service.
- The devkitA64 runtime includes newlib and GCC/libstdc++ components with
  their own licenses and runtime exceptions. The inherited NVK link also
  names Expat (MIT). Complete matching toolchain, Mesa and Expat notices and
  source provenance have not been assembled in this limited license collection.
  UAM's top-level license does not replace its imported Mesa per-file notices.

## Changes to third-party code and LGPL components

The historical `patches/` material records SDK, FFmpeg and XenosRecomp changes
against older bases. It must not be read as an exhaustive delta for this
preview binary. The inherited fetch script refers to the ReXGlue v0.10.0 base
`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`; that does not pin every current
local dependency or subsequent modification.

Recorded FFmpeg integration work includes `libavcodec/fft_template.c`,
`libavcodec/mdct_template.c`, `config.h`, `config_switch_aarch64.h`,
`config_masseffect_wmv3.h`, the codec registration overlay and the SDK's
third-party CMake configuration. The WMV3 integration was inherited from
masseffect-nx; its name does not establish which codecs this FC3 binary uses.
FFmpeg's upstream license overview describes optional components that can
change its licensing. The actual selected files and configuration need to be
checked against the release binary.

FFmpeg and libmspack retain their LGPL terms. Distributing their license texts
alone does not supply the matching library source, modifications, or materials
needed to exercise applicable modification and relinking rights. No claim is
made that those requirements are fulfilled by this source snapshot, nor that
the remaining library code is necessarily unmodified.

## Public license-text provenance

Retrieved through the GitHub API on 2026-11-23 (Git blob identifiers identify
the license text, not the installed library revision):

- [libnx LICENSE.md](https://github.com/switchbrew/libnx/blob/master/LICENSE.md):
  `b8327e3f77c35baf0840797b542c67e5918367fe`.
- [tiny-AES-c unlicense.txt](https://github.com/kokke/tiny-AES-c/blob/master/unlicense.txt):
  `68a49daad8ff7e35068f2b7a97d643aab440eaec`.
- [Disruptor++ LICENSE.txt](https://github.com/lewissbaker/disruptorplus/blob/master/LICENSE.txt):
  `f240740b6bfc433f6d63e056f41cf36c1220ea83`.

## Original game rights and trademarks

The open-source licenses above do **not** grant rights to Far Cry 3's original
game code, recompiled or otherwise game-derived code, translated game shaders,
artwork, audio or other game assets. Those rights are separate and remain with
their respective rights holders. Omitting a game-data directory does not by
itself establish permission to distribute game-derived material in a binary
or shader library.

The release instructions require the user's own lawfully obtained Xbox 360
copy of Far Cry 3 and locally extracted game files. These notices do not
provide a game license or authorize redistribution of the original game.

Far Cry and Ubisoft are trademarks of Ubisoft. Nintendo Switch is a trademark
of Nintendo. Xbox, Xbox 360 and Xbox Live are trademarks of Microsoft. This
project is not affiliated with or endorsed by Ubisoft, Nintendo or Microsoft.
