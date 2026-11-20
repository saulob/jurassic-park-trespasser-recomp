# Direct3D 6 inventory of trespass.exe

What the game really calls on its Direct3D path, recovered from the lifted code (`src/recomp/gen`,
whose comments carry the original instructions) and checked against the vtable layouts in the
Windows SDK's `d3d.h`. This is what `src/host/ddraw/d3d_objects.c` and `d3d11_renderer.c` implement.

## How the inventory was made

1. The executable links `dxguid.lib`, so every DirectX GUID is in `.rdata`; only these are referenced
   by code: `IID_IDirectDraw2` (0x6482E0), `IID_IDirectDraw4` (0x6482F0), `IID_IDirectDrawSurface`
   (0x648300), `IID_IDirectDrawGammaControl` (0x6483D0), `IID_IDirect3D3` (0x648610) and
   `IID_IDirect3DTexture2` (0x6486C0). No other Direct3D interface (IDirect3D/2, Device/2,
   Viewport/2, Material, Light, VertexBuffer) is ever requested.
2. Every `call dword ptr [reg + off]` was traced back to the object whose vtable it loads. The
   Direct3D driver object (`d3dDriver`) is the global at 0x6D0E98: `+4` IDirect3DDevice3
   (0x6D0E9C), `+8` IDirect3D3 (0x6D0EA0), `+0x88` IDirect3DViewport3. IDirectDraw2 is
   0x6D0E48 and IDirectDraw4 0x6D0E4C.
3. Arguments were read from the pushes before each call (render state numbers, primitive type,
   FVF, flags).

Call sites are lifted functions (`sub_XXXXXXXX`, written without the prefix). "dxwrapper" refers to
https://github.com/elishacloud/dxwrapper (`ddraw/` sources, zlib-style licence); it was used for
concepts only, no code was copied. dgVoodoo2 is closed source (its repository holds only a
README), so it is a behavioural reference only; "-" means it adds nothing specific.

## IDirect3D3 (face of the DirectDraw object; QueryInterface from IDirectDraw4)

| Slot | Method | Call sites | Observed use | Status | dxwrapper | dgVoodoo2 |
|---|---|---|---|---|---|---|
| 0x00 | QueryInterface | 00505AF0 (on IDirectDraw4), 00408850 | obtains IDirect3D3 | implemented | IDirect3DX.cpp | - |
| 0x08 | Release | 00505B50, 00408850 | shutdown | implemented (count shared with DirectDraw) | IDirect3DX.cpp | - |
| 0x0C | EnumDevices | 00408850 (video options card test) | callback 00408530 checks the HAL caps | implemented: one HAL device that passes the game's test | IDirect3DX.cpp | D3D6 device caps |
| 0x18 | CreateViewport | 005063B0 | one viewport | implemented | IDirect3DViewportX.cpp | - |
| 0x20 | CreateDevice | 00505B80 | GUID from registry "D3D GUID", back buffer | implemented (any GUID -> D3D11 device) | IDirect3DX.cpp | D3D6 on D3D11 |
| 0x28 | EnumZBufferFormats | 00504F70 | callback 00504FF0 keeps the deepest pure Z format | implemented: 16 and 24 bit | IDirect3DX.cpp | - |
| 0x2C | EvictManagedTextures | 00506130 | with "Page Managed" | implemented (no-op: textures live in host memory) | IDirect3DX.cpp | - |
| other | CreateLight, CreateMaterial, FindDevice, CreateVertexBuffer | none | never called | refused with DDERR_UNSUPPORTED and one log line | | |

## IDirect3DDevice3

| Slot | Method | Call sites | Observed use | Status | dxwrapper | dgVoodoo2 |
|---|---|---|---|---|---|---|
| 0x08 | Release | 00505D70 | shutdown | implemented | IDirect3DDeviceX.cpp | - |
| 0x0C | GetCaps | 00506950 | reads the HAL caps | implemented | IDirect3DDeviceX.cpp | - |
| 0x14 | AddViewport | 005063B0 | | implemented | IDirect3DDeviceX.cpp | - |
| 0x20 | EnumTextureFormats | 005064D0 | callback 00513590: 16-bit opaque, 1-bit alpha, most alpha | implemented: RGB565, ARGB1555, ARGB4444 | IDirect3DDeviceX.cpp | - |
| 0x24 | BeginScene | 005242C0, 00555B00, 00513740, 00513C80 | many scenes per frame | implemented | IDirect3DDeviceX.cpp | - |
| 0x28 | EndScene | 005242C0, 00555A40, 00555B00, 00513740, 00513C80 | | implemented (flushes the batch) | IDirect3DDeviceX.cpp | - |
| 0x30 | SetCurrentViewport | 005063B0 | | implemented | IDirect3DDeviceX.cpp | - |
| 0x58 | SetRenderState | 133 sites in 00516190, 00516350, 005143A0, 005062D0, 005242C0 and 00555B00-0055D8B0 | see the state table below | implemented for every observed state | IDirect3DDeviceX.cpp | fixed function -> shaders |
| 0x70 | DrawPrimitive | 00556CE0, 00556FC0, 00557230, 00557BD0, 005585A0, 005588D0, 00559240, 00559C70, 00559EC0, 0055A1A0, 0055B560, 0055B891, 0055C1C4, 0055CC80, 0055CF50, 0055D430, 0055D8B0, 00513B70 | always `D3DPT_TRIANGLEFAN`, FVF 0x1C4 (`D3DFVF_TLVERTEX`), flags 0x1C (DONOTCLIP, DONOTUPDATEEXTENTS, DONOTLIGHT) | implemented: fans -> indexed lists, batched | IDirect3DDeviceX.cpp, Shaders/FixUpVertexShader | TL vertices |
| 0x74 | DrawIndexedPrimitive | none | not used | implemented (triangle types, points, lines) | IDirect3DDeviceX.cpp | - |
| 0x98 | SetTexture | 39 sites in 20 functions (00513740, 00513F80, 00516190, 005143A0, 005242C0, 00555A40-0055D8B0) | stage 0 draws; 00516190 also clears stages 1-7 (NULL) | implemented for stages 0-7 | IDirect3DDeviceX.cpp | - |
| 0xA0 | SetTextureStageState | 62 sites in 16 functions (00516190, 005143A0, 005242C0, 00555BB0-0055D8B0) | stage 0: ADDRESS, ADDRESSU, ADDRESSV, MAGFILTER, MINFILTER; 00516190 sets ADDRESS=CLAMP, MAGFILTER=POINT, MINFILTER=POINT on stages 0-7 | implemented for stages 0-7 (see below) | IDirect3DDeviceX.cpp | sampler states |
| 0x64, 0x68, 0x6C | Set/Get/MultiplyTransform | none | not used (the game transforms on the CPU) | stored and returned; drawing untransformed vertices is refused | IDirect3DDeviceX.cpp | - |
| other | GetStats, Begin/Vertex/End, light states, clip status, strided and VB draws, ComputeSphereVisibility, SetRenderTarget | none | never called | refused with DDERR_UNSUPPORTED and one log line | | |

### Render states (Direct3D 6 numbering) and their translation

| State | Values seen | D3D11 translation |
|---|---|---|
| 4 TEXTUREPERSPECTIVE | 1 | always perspective-correct |
| 7 ZENABLE | 0, 1, register | depth test on the D24S8 buffer when a z-buffer is attached |
| 8 FILLMODE | 3 (solid) | rasterizer fill mode (wireframe also mapped) |
| 9 SHADEMODE | 1 flat, 2 Gouraud | `nointerpolation` colour (Direct3D's first-vertex rule) or interpolated |
| 14 ZWRITEENABLE | 1, register | depth write mask |
| 15 ALPHATESTENABLE | 0, 1, register | `discard` in the pixel shader |
| 19/20 SRC/DESTBLEND | 5 SRCALPHA, 6 INVSRCALPHA | blend state (cached) |
| 21 TEXTUREMAPBLEND | 1 DECAL, 2 MODULATE | pixel shader combine, Direct3D 6 legacy rules for all eight modes (`d3dt_texture_blend` is the tested reference): DECAL/COPY = texture (Wine: SELECTARG1 TEXTURE); MODULATE = texture x diffuse, alpha from the texture if it has alpha else the vertex; DECALALPHA = lerp by texture alpha, vertex alpha; MODULATEALPHA = texture x diffuse including alpha; DECALMASK/MODULATEMASK = texture (x diffuse) where the texture alpha low bit is set, else diffuse, vertex alpha (DX6 spec; Wine and dxwrapper leave them unimplemented); ADD = saturated texture + diffuse, vertex alpha. The terrain batch (0055D8B0) uses DECAL and resets to MODULATE |
| 22 CULLMODE | 1 none | rasterizer cull mode |
| 23 ZFUNC | 7 GREATEREQUAL, 8 ALWAYS | depth function (sz grows with 1/z; cleared to 0) |
| 24 ALPHAREF | 0x1000 | low byte (0): "alpha > 0" |
| 25 ALPHAFUNC | 5 GREATER | alpha test comparison |
| 26 DITHERENABLE | 0, register | accepted; irrelevant on an 8-bit-per-channel target |
| 27 ALPHABLENDENABLE | 0, 1, register | blend state |
| 28 FOGENABLE | 0, 1, register | vertex fog from specular alpha |
| 29 SPECULARENABLE | 0, 1, register | adds specular RGB |
| 31 SUBPIXEL | 1 | accepted (always sub-pixel) |
| 34 FOGCOLOR | register | fog colour constant |
| 35 FOGTABLEMODE | 0 none | only NONE is accepted |
| 41 COLORKEYENABLE | register | colour-keyed texels discarded |
| 47 ZBIAS | 0 (00516190, 005143A0) | `DepthBias` in the rasterizer state: 84 units of 2^-24 per step (0-16), i.e. 5e-6 of depth per step as Direct3D 7-era drivers and wined3d apply it, towards the viewer of the current ZFUNC (positive for GREATER/GREATEREQUAL). Values above 16 are refused. |
| 48 RANGEFOGENABLE | not seen | accepted: it only changes how Direct3D computes vertex fog for untransformed vertices; the game's TL vertices carry their own fog factor in specular alpha |

Any other state, or an invalid value, is refused (DDERR_INVALIDPARAMS), logged once with its
D3DRENDERSTATE_ name and caller, and counted (`unsupported_render_states` at the end of the log).
An earlier version of this table called state 47 RANGEFOGENABLE; `d3dtypes.h` defines 47 as ZBIAS
and 48 as RANGEFOGENABLE.

### Texture stage states

| Type | Values seen | Translation |
|---|---|---|
| 12 ADDRESS, 13 ADDRESSU, 14 ADDRESSV | WRAP 1, CLAMP 3 (MIRROR 2 also mapped; BORDER refused) | sampler address mode, stage 0 |
| 16 MAGFILTER, 17 MINFILTER, 18 MIPFILTER | POINT 1, LINEAR 2 | sampler filter, stage 0 (`--texture-filter` where the game asks for linear) |
| 1 COLOROP, 4 ALPHAOP | not seen | accepted only as DISABLE on stages 1-7 (their default); stage 0 combining is TEXTUREMAPBLEND |
| 11 TEXCOORDINDEX | not seen | accepted only as the stage's own index |

Stages 1-7 store their states; their COLOROP is DISABLE, as in Direct3D 6, so only stage 0 draws.
Until this was implemented every call for stages 1-7 (21 per level start, from 00516190) was refused,
logged under the type number only (12, 16, 17), which made them look like unknown states.

## IDirect3DViewport3

| Slot | Method | Call sites | Observed use | Status | dxwrapper | dgVoodoo2 |
|---|---|---|---|---|---|---|
| 0x08 | Release | 00505D70 | | implemented | IDirect3DViewportX.cpp | - |
| 0x44 | SetViewport2 | 005063B0 | 0,0,w,h; z 0..1 | implemented: becomes the scissor rectangle | IDirect3DViewportX.cpp | - |
| 0x50 | Clear2 | 005069E0, 00513740 | TARGET and ZBUFFER, fog colour, z = 0 | implemented: clear views, or a quad for partial rectangles | IDirect3DViewportX.cpp | - |
| other | | none | never called | refused with one log line | | |

## IDirect3DTexture2 (face of a texture surface; QueryInterface from IDirectDrawSurface4)

| Slot | Method | Call sites | Observed use | Status | dxwrapper | dgVoodoo2 |
|---|---|---|---|---|---|---|
| 0x00 | QueryInterface | 00510240, 00513740, 00513C80 | from both surfaces of a texture | implemented | IDirect3DTextureX.cpp | - |
| 0x08 | Release | 00513740, 00513C80 | | implemented (count shared with the surface) | IDirect3DTextureX.cpp | - |
| 0x14 | Load | 00510240, 00510320, 00513740, 00513C80 | system-memory copy -> device copy | implemented: immediate copy, texture marked dirty | IDirect3DTextureX.cpp | - |
| 0x0C, 0x10 | GetHandle, PaletteChanged | none | never called | refused | | |

## DirectDraw calls that belong to the Direct3D path

| Interface | Slot | Method | Call sites | Use |
|---|---|---|---|---|
| IDirectDraw4 | 0x18 | CreateSurface | 00505030 (z-buffer, caps 0x24000), 00510560, 00510640, 00513740, 00513C80 (textures 0x1800 / 0x5000) | z-buffer and texture surfaces |
| IDirectDraw4 | 0x5C | GetAvailableVidMem | 00507080, 00506DD0 | texture budget (DDSCAPS_TEXTURE, LOCALVIDMEM) |
| IDirectDraw4 | 0x6C | GetDeviceIdentifier | 005024C0, 00506950 | card quirks by vendor; an unknown vendor is "other" |
| IDirectDrawSurface4 | 0x0C | AddAttachedSurface | 00505030 | z-buffer onto the back buffer |
| IDirectDrawSurface4 | 0x58 | GetSurfaceDesc | 005063B0, 00505030, 00506AD0 | sizes and the 555/565 test |
| IDirectDrawSurface4 | 0x64, 0x80 | Lock, Unlock | 00513660, 00513FA0, 00514070, 00514150 | texture writes, and the start-up test that reads the back buffer back |

## Textures, formats, palettes, colour keys

- Formats: RGB565 (opaque), ARGB1555 (one-bit transparency) and ARGB4444 (alpha). No palettised
  (P8) textures and no mipmaps are created by the game.
- Every texture is a pair: a system-memory surface the CPU writes through Lock, and a device
  surface filled with `Load` (or a partial `Blt`).
- Colour keys exist in the code but the shipping game draws transparency with ARGB1555 and alpha
  test; source colour keys are still honoured (keyed texels get alpha 0, `COLORKEYENABLE` discards them).

## Back-buffer coherence

The game interleaves software rasterisation into the locked back buffer with Direct3D scenes,
draws text with GetDC, and at start-up renders test polygons and reads them back with Lock
(00513740 -> 00513660) to calibrate its pixel offsets. The host therefore reads the GPU target
back on a Lock or GetDC that follows GPU drawing, and composites the pixels the CPU changed over
the GPU target before the next GPU draw or the Flip.
