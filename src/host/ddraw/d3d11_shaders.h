#ifndef TRESPASSER_D3D11_SHADERS_H
#define TRESPASSER_D3D11_SHADERS_H

enum {
    D3D11_SHADER_VS_TL,
    D3D11_SHADER_PS_FIXED,
    D3D11_SHADER_VS_QUAD,
    D3D11_SHADER_PS_FILL,
    D3D11_SHADER_PS_COPY,
    D3D11_SHADER_PS_PRESENT,
    D3D11_SHADER_PS_PACK565,
    D3D11_SHADER_COUNT
};

/* Pixel shader flags; the HLSL defines the same values. */
enum {
    D3D11_PS_TEXTURED = 1,
    D3D11_PS_TEX_ALPHA = 2,
    D3D11_PS_KEY_TEST = 4,
    D3D11_PS_ALPHA_TEST = 8,
    D3D11_PS_FOG = 16,
    D3D11_PS_SPECULAR = 32,
    D3D11_PS_FLAT = 64,
    D3D11_PS_DEBUG_TEXTURE = 128,
    D3D11_PS_DEBUG_VERTEX = 256,
    D3D11_PS_DEBUG_DEPTH = 512,
    D3D11_PS_DEBUG_SOLID = 1024,
    D3D11_PS_DEBUG_WIRE = 2048
};

typedef struct {
    const char* entry;
    const char* profile;
} d3d11_shader_entry_t;

extern const char d3d11_shader_source[];
extern const d3d11_shader_entry_t d3d11_shader_entries[D3D11_SHADER_COUNT];

#endif
