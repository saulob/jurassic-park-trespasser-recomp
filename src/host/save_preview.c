#include "save_preview.h"

#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#else
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL_surface.h>

#include "guest_abi.h"

#define MAX_PATH 1024
#endif

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_DIR "previews"
#ifdef _WIN32
#define PREVIEW_EXT ".jpg"
#else
#define PREVIEW_EXT ".png"
#endif
#define JPEG_QUALITY 0.9f
#define MAX_SIDE 4096

sp_size_t save_preview_size(int screen_w, int screen_h) {
    sp_size_t s = { 0, 0 };
    double k;
    if (screen_w <= 0 || screen_h <= 0) return s;
    k = sqrt((double)SAVE_PREVIEW_PIXELS / ((double)screen_w * screen_h));
    if (k > 1.0) k = 1.0;
    s.width = (int)(screen_w * k + 0.5);
    s.height = (int)(screen_h * k + 0.5);
    if (s.width < 1) s.width = 1;
    if (s.height < 1) s.height = 1;
    return s;
}

static const char* file_part(const char* path) {
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    return a > b ? a : b;
}

int save_preview_path(const char* save_path, char* out, size_t cap) {
    const char* slash = save_path ? file_part(save_path) : NULL;
    int n;
    if (!slash || !slash[1] || !cap) return 0;
    n = _snprintf(out, cap, "%.*s\\%s\\%s%s", (int)(slash - save_path), save_path, PREVIEW_DIR, slash + 1, PREVIEW_EXT);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

int save_preview_is_save(const char* path, const char* user_dir, const char* base) {
    size_t d = user_dir ? strlen(user_dir) : 0, b = base ? strlen(base) : 0;
    const char* name;
    if (!path || !d || !b || _strnicmp(path, user_dir, d)) return 0;
    name = path + d;
    if (user_dir[d - 1] != '\\' && user_dir[d - 1] != '/') {
        if (*name != '\\' && *name != '/') return 0;
        name++;
    }
    if (_strnicmp(name, base, b) || name[b] != '.') return 0;
    name += b + 1;
    if (!_stricmp(name, "QCK")) return 1;
    return isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) && isdigit((unsigned char)name[2]) && !name[3];
}

/* Area average: every screen pixel counts for its share of each picture pixel it falls in. */
static uint8_t* downscale(const uint16_t* src, int sw, int sh, long pitch, int dw, int dh) {
    float* acc = (float*)calloc((size_t)dw * dh * 3, sizeof(float));
    float* row = (float*)malloc((size_t)dw * 3 * sizeof(float));
    uint8_t* out = (uint8_t*)malloc((size_t)dw * dh * 3);
    double kx = (double)sw / dw, ky = (double)sh / dh;
    if (!acc || !row || !out) {
        free(acc);
        free(row);
        free(out);
        return NULL;
    }
    for (int sy = 0; sy < sh; sy++) {
        const uint16_t* in = src + (long)sy * pitch;
        int y0 = (int)(sy / ky), y1 = (int)((sy + 1) / ky);
        memset(row, 0, (size_t)dw * 3 * sizeof(float));
        for (int x = 0; x < dw; x++) {
            double f0 = x * kx, f1 = (x + 1) * kx;
            for (int sx = (int)f0; sx < sw && sx < f1; sx++) {
                double w = (sx + 1 < f1 ? sx + 1 : f1) - (sx > f0 ? sx : f0);
                uint16_t v = in[sx];
                row[x * 3 + 0] += (float)(w * ((v & 31) * 255.0 / 31.0));
                row[x * 3 + 1] += (float)(w * (((v >> 5) & 63) * 255.0 / 63.0));
                row[x * 3 + 2] += (float)(w * (((v >> 11) & 31) * 255.0 / 31.0));
            }
        }
        for (int y = y0; y <= y1 && y < dh; y++) {
            double top = y * ky > sy ? y * ky : sy, bottom = (y + 1) * ky < sy + 1 ? (y + 1) * ky : sy + 1;
            float w = (float)(bottom - top);
            if (w <= 0) continue;
            for (int i = 0; i < dw * 3; i++) acc[(size_t)y * dw * 3 + i] += row[i] * w;
        }
    }
    for (size_t i = 0; i < (size_t)dw * dh * 3; i++) {
        float v = acc[i] / (float)(kx * ky);
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
    }
    free(acc);
    free(row);
    return out;
}

#ifdef _WIN32
/* 1: this call initialised COM and must uninitialise it; 0: the thread already had it; -1: COM is unavailable. */
static int com_enter(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) return 1;
    return hr == RPC_E_CHANGED_MODE ? 0 : -1;
}

static void com_leave(int entered) {
    if (entered == 1) CoUninitialize();
}

static IWICImagingFactory* wic_factory(void) {
    IWICImagingFactory* f = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&f)))
        return NULL;
    return f;
}

static int wide(const char* path, WCHAR* out, int cap) { return MultiByteToWideChar(CP_ACP, 0, path, -1, out, cap) > 0; }

static int encode_jpeg(const char* path, const uint8_t* bgr, int w, int h) {
    IWICImagingFactory* f = wic_factory();
    IWICStream* stream = NULL;
    IWICBitmapEncoder* encoder = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IPropertyBag2* props = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    WCHAR wpath[MAX_PATH];
    PROPBAG2 option;
    VARIANT quality;
    HRESULT hr = E_FAIL;
    if (!f || !wide(path, wpath, MAX_PATH)) goto done;
    hr = IWICImagingFactory_CreateStream(f, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream*)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &props);
    if (SUCCEEDED(hr)) {
        memset(&option, 0, sizeof option);
        option.pstrName = L"ImageQuality";
        memset(&quality, 0, sizeof quality);
        V_VT(&quality) = VT_R4;
        V_R4(&quality) = JPEG_QUALITY;
        IPropertyBag2_Write(props, 1, &option, &quality);
        hr = IWICBitmapFrameEncode_Initialize(frame, props);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, (UINT)w, (UINT)h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr) && !IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(frame, (UINT)h, (UINT)w * 3, (UINT)(w * h * 3), (BYTE*)bgr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
done:
    if (props) IPropertyBag2_Release(props);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (f) IWICImagingFactory_Release(f);
    return SUCCEEDED(hr);
}

/* The picture goes to a temporary file first, so a failed write never leaves half a picture for a save. */
int save_preview_write(const char* save_path, const uint16_t* rgb565, int w, int h, long pitch) {
    char path[MAX_PATH], temp[MAX_PATH + 8], dir[MAX_PATH];
    sp_size_t size = save_preview_size(w, h);
    uint8_t* bgr;
    int entered, ok = 0;
    if (!rgb565 || w <= 0 || h <= 0 || pitch < w || !save_preview_path(save_path, path, sizeof path)) return 0;
    strcpy(dir, path);
    *(char*)file_part(dir) = 0;
    CreateDirectoryA(dir, NULL);
    _snprintf(temp, sizeof temp - 1, "%s.tmp", path);
    temp[sizeof temp - 1] = 0;
    bgr = downscale(rgb565, w, h, pitch, size.width, size.height);
    if (!bgr) return 0;
    entered = com_enter();
    if (entered >= 0) {
        ok = encode_jpeg(temp, bgr, size.width, size.height) && MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING);
        com_leave(entered);
    }
    if (!ok) DeleteFileA(temp);
    free(bgr);
    return ok;
}

#else

/* A save's picture is a PNG beside the save, written through a temporary file; paths are the game's own and resolve to host files. */
static int host_file(const char* guest_path, char* out, size_t cap) { return guest_host_path(guest_path, out, cap); }

/* Where a save's picture is written: the save's own folder resolves (it exists), the picture goes in its previews folder, which is made. */
static int host_new_preview(const char* save_path, char* out, size_t cap) {
    const char* slash = file_part(save_path);
    char folder[MAX_PATH], host_folder[MAX_PATH];
    size_t n, keep;
    if (!slash || !slash[1]) return 0;
    n = (size_t)(slash - save_path) + 1;
    if (n >= sizeof folder) return 0;
    memcpy(folder, save_path, n);
    folder[n] = 0;
    if (!host_file(folder, host_folder, sizeof host_folder)) return 0;
    keep = strlen(host_folder);
    while (keep > 1 && host_folder[keep - 1] == '/') host_folder[--keep] = 0;
    if (snprintf(out, cap, "%s/%s", host_folder, PREVIEW_DIR) >= (int)cap) return 0;
    mkdir(out, 0777);
    return snprintf(out, cap, "%s/%s/%s%s", host_folder, PREVIEW_DIR, slash + 1, PREVIEW_EXT) < (int)cap;
}

static int bgr_surface(uint8_t* bgr, int w, int h, SDL_Surface** out) {
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_BGR24);
    if (!s) return 0;
    for (int y = 0; y < h; y++) memcpy((uint8_t*)s->pixels + (size_t)y * (size_t)s->pitch, bgr + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    *out = s;
    return 1;
}

int save_preview_write(const char* save_path, const uint16_t* rgb565, int w, int h, long pitch) {
    char host[MAX_PATH], temp[MAX_PATH + 8];
    sp_size_t size = save_preview_size(w, h);
    uint8_t* bgr;
    SDL_Surface* surface = NULL;
    int ok = 0;
    if (!rgb565 || w <= 0 || h <= 0 || pitch < w || !host_new_preview(save_path, host, sizeof host)) return 0;
    snprintf(temp, sizeof temp, "%s.tmp", host);
    bgr = downscale(rgb565, w, h, pitch, size.width, size.height);
    if (!bgr) return 0;
    if (bgr_surface(bgr, size.width, size.height, &surface)) {
        ok = SDL_SavePNG(surface, temp) && rename(temp, host) == 0;
        SDL_DestroySurface(surface);
    }
    if (!ok) unlink(temp);
    free(bgr);
    return ok;
}
#endif

int save_preview_shrink(const uint16_t* rgb565, int w, int h, long pitch, sp_image_t* out) {
    sp_size_t size = save_preview_size(w, h);
    uint8_t* bgr;
    memset(out, 0, sizeof *out);
    if (!rgb565 || w <= 0 || h <= 0 || pitch < w) return 0;
    bgr = downscale(rgb565, w, h, pitch, size.width, size.height);
    out->pixels = bgr ? (uint32_t*)malloc((size_t)size.width * size.height * 4) : NULL;
    if (!out->pixels) {
        free(bgr);
        return 0;
    }
    for (size_t i = 0; i < (size_t)size.width * size.height; i++)
        out->pixels[i] = 0xFF000000u | ((uint32_t)bgr[i * 3 + 2] << 16) | ((uint32_t)bgr[i * 3 + 1] << 8) | bgr[i * 3];
    out->width = size.width;
    out->height = size.height;
    free(bgr);
    return 1;
}

#ifdef _WIN32
int save_preview_delete(const char* save_path) {
    char path[MAX_PATH];
    if (!save_preview_path(save_path, path, sizeof path)) return 0;
    return DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

#define MAX_ART_SIDE 16384

/* fit_w and fit_h 0: the picture as stored, at most MAX_SIDE a side; otherwise shrunk to fit them. */
static int decode(const char* path, int fit_w, int fit_h, sp_image_t* out) {
    IWICImagingFactory* f = wic_factory();
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICBitmapScaler* scaler = NULL;
    IWICFormatConverter* converter = NULL;
    IWICBitmapSource* source = NULL;
    WCHAR wpath[MAX_PATH];
    UINT w = 0, h = 0, limit = fit_w > 0 && fit_h > 0 ? MAX_ART_SIDE : MAX_SIDE;
    HRESULT hr = E_FAIL;
    if (!f || !wide(path, wpath, MAX_PATH)) goto done;
    hr = IWICImagingFactory_CreateDecoderFromFilename(f, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > limit || h > limit)) hr = E_FAIL;
    source = (IWICBitmapSource*)frame;
    if (SUCCEEDED(hr) && fit_w > 0 && fit_h > 0 && (w > (UINT)fit_w || h > (UINT)fit_h)) {
        double k = (double)fit_w / w < (double)fit_h / h ? (double)fit_w / w : (double)fit_h / h;
        UINT sw = (UINT)(w * k + 0.5), sh = (UINT)(h * k + 0.5);
        w = sw ? sw : 1;
        h = sh ? sh : 1;
        hr = IWICImagingFactory_CreateBitmapScaler(f, &scaler);
        if (SUCCEEDED(hr)) hr = IWICBitmapScaler_Initialize(scaler, source, w, h, WICBitmapInterpolationModeFant);
        source = (IWICBitmapSource*)scaler;
    }
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &converter);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(converter, source, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                            WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        out->pixels = (uint32_t*)malloc((size_t)w * h * 4);
        hr = out->pixels ? IWICFormatConverter_CopyPixels(converter, NULL, w * 4, w * h * 4, (BYTE*)out->pixels) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        out->width = (int)w;
        out->height = (int)h;
        for (size_t i = 0; i < (size_t)w * h; i++) out->pixels[i] |= 0xFF000000u;
    }
done:
    if (converter) IWICFormatConverter_Release(converter);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (f) IWICImagingFactory_Release(f);
    if (FAILED(hr)) save_preview_free(out);
    return SUCCEEDED(hr);
}

int save_preview_load(const char* save_path, sp_image_t* out) {
    char path[MAX_PATH];
    int entered, ok = 0;
    memset(out, 0, sizeof *out);
    if (!save_preview_path(save_path, path, sizeof path) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    entered = com_enter();
    if (entered < 0) return 0;
    ok = decode(path, 0, 0, out);
    com_leave(entered);
    return ok;
}

int sp_image_load(const char* path, int fit_w, int fit_h, sp_image_t* out) {
    int entered, ok;
    memset(out, 0, sizeof *out);
    if (!path || fit_w <= 0 || fit_h <= 0 || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    entered = com_enter();
    if (entered < 0) return 0;
    ok = decode(path, fit_w, fit_h, out);
    com_leave(entered);
    return ok;
}

#else

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_FAILURE_USERMSG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define MAX_ART_SIDE 16384

int save_preview_delete(const char* save_path) {
    char path[MAX_PATH], host[MAX_PATH];
    if (!save_preview_path(save_path, path, sizeof path) || !guest_host_path(path, host, sizeof host)) return 0;
    return unlink(host) == 0;
}

static uint8_t* read_file(const char* host, size_t* size) {
    FILE* f = fopen(host, "rb");
    uint8_t* data;
    long length;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) <= 0 || length > (64L << 20) || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    data = (uint8_t*)malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return data;
}

/* The picture as opaque BGRA, area-averaged down to fit fit_w x fit_h when it is larger (fit 0: as stored, at most MAX_SIDE). */
static int decode(const char* host, int fit_w, int fit_h, sp_image_t* out) {
    size_t size = 0;
    uint8_t *file = read_file(host, &size), *rgba;
    int w = 0, h = 0, n = 0, limit = fit_w > 0 && fit_h > 0 ? MAX_ART_SIDE : MAX_SIDE, dw, dh;
    if (!file) return 0;
    rgba = stbi_load_from_memory(file, (int)size, &w, &h, &n, 4);
    free(file);
    if (!rgba) return 0;
    if (w <= 0 || h <= 0 || w > limit || h > limit) {
        stbi_image_free(rgba);
        return 0;
    }
    dw = w;
    dh = h;
    if (fit_w > 0 && fit_h > 0 && (w > fit_w || h > fit_h)) {
        double k = (double)fit_w / w < (double)fit_h / h ? (double)fit_w / w : (double)fit_h / h;
        dw = (int)(w * k + 0.5);
        dh = (int)(h * k + 0.5);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    out->pixels = (uint32_t*)malloc((size_t)dw * dh * 4);
    if (!out->pixels) {
        stbi_image_free(rgba);
        return 0;
    }
    for (int y = 0; y < dh; y++) {
        const int y0 = (int)((long long)y * h / dh), y1 = (int)(((long long)y + 1) * h / dh) > y0 ? (int)(((long long)y + 1) * h / dh) : y0 + 1;
        for (int x = 0; x < dw; x++) {
            const int x0 = (int)((long long)x * w / dw), x1 = (int)(((long long)x + 1) * w / dw) > x0 ? (int)(((long long)x + 1) * w / dw) : x0 + 1;
            unsigned sum[3] = { 0, 0, 0 }, count = 0;
            for (int sy = y0; sy < y1 && sy < h; sy++)
                for (int sx = x0; sx < x1 && sx < w; sx++) {
                    const uint8_t* p = rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], count++;
                }
            if (!count) count = 1;
            out->pixels[(size_t)y * dw + x] = 0xFF000000u | ((sum[0] / count) << 16) | ((sum[1] / count) << 8) | (sum[2] / count);
        }
    }
    out->width = dw;
    out->height = dh;
    stbi_image_free(rgba);
    return 1;
}

int save_preview_load(const char* save_path, sp_image_t* out) {
    char path[MAX_PATH], host[MAX_PATH];
    memset(out, 0, sizeof *out);
    if (!save_preview_path(save_path, path, sizeof path) || !guest_host_path(path, host, sizeof host)) return 0;
    return decode(host, 0, 0, out);
}

int sp_image_load(const char* path, int fit_w, int fit_h, sp_image_t* out) {
    char host[MAX_PATH];
    memset(out, 0, sizeof *out);
    if (!path || fit_w <= 0 || fit_h <= 0) return 0;
    if (!guest_host_path(path, host, sizeof host)) snprintf(host, sizeof host, "%s", path);
    return decode(host, fit_w, fit_h, out);
}
#endif

void save_preview_free(sp_image_t* image) {
    free(image->pixels);
    image->pixels = NULL;
    image->width = image->height = 0;
}
