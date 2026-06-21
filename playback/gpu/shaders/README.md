# GPU shader toolchain

Portable shader spine for the GPU-resident pipeline. One GLSL source set is
cross-compiled to every backend shading language and packed into `.qsb`
containers; the RHI consumes the `.qsb` at runtime.

## Source layout

- `*.vert` / `*.frag` are GLSL 440 source files.
- `qt_add_shaders(<target> ...)` in `tests/CMakeLists.txt`, under
  `OLR_GPU_PIPELINE`, runs `qsb` to bake each source into a multi-target `.qsb`
  and embeds it at `:/olr/shaders/<name>.qsb`.
- `olr_shaders.qrc` documents the embedded resource layout. The runtime
  resource is produced by `qt_add_shaders`, not this `.qrc`.

## Baking

`qsb --qt6 <source>` is the default target set: GLSL, HLSL, MSL, and SPIR-V.
Inspect a baked artifact with `qsb --dump <file>.qsb`; the header lists every
shading language present, which the CI shader-compile gate asserts.

## Runtime load

`QShader::fromSerialized(QByteArray)` deserializes a `.qsb` into a `QShader`.
`OlrRhi` (`playback/gpu/olrrhi.h`) builds a `QRhiGraphicsPipeline` from the
vertex and fragment `QShader` pair.

## Backend-selection seam

`OlrRhi::Backend` is `Null` only today: the deterministic, headless, GPU-free
backend that CI runs on. The GPU abstraction widens it to `Metal` on Apple
platforms and `D3D11` on Windows behind the same `OlrRhi::create` factory, with
one `QRhi` owned by one render thread. The Null backend stays as the CI oracle
and the RHI-unavailable fallback target.

## Qt-version coupling

QRhi and `.qsb` have limited cross-Qt-version compatibility guarantees, so the
GPU pipeline is pinned to the project's Qt 6.10.x toolchain. Re-bake `.qsb`
artifacts whenever the Qt minor version changes.
