#include "jpt_renderer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "jpt_compat.h"

static const char* const k_keys[JPT_RENDERER_COUNT] = { "auto", "d3d11", "vulkan", "software" };
static const char* const k_labels[JPT_RENDERER_COUNT] = { "Auto", "Direct3D 11", "Vulkan", "Software" };

static void set_reason(char* dst, size_t cap, const char* text) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", text ? text : "");
}

const char* jpt_renderer_key(jpt_renderer_t renderer) {
    if (renderer < 0 || renderer >= JPT_RENDERER_COUNT) return "auto";
    return k_keys[renderer];
}

const char* jpt_renderer_label(jpt_renderer_t renderer) {
    if (renderer < 0 || renderer >= JPT_RENDERER_COUNT) return "Auto";
    return k_labels[renderer];
}

const char* jpt_renderer_source_name(jpt_renderer_source_t source) {
    switch (source) {
    case JPT_RENDERER_SOURCE_INI: return "ini";
    case JPT_RENDERER_SOURCE_ENV: return "env";
    case JPT_RENDERER_SOURCE_CLI: return "cli";
    default: return "default";
    }
}

void jpt_renderer_select_request(int ini_present, jpt_renderer_t ini_value, const char* env_text,
                                 const char* cli_text, jpt_renderer_selection_t* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->ini_present = ini_present ? 1 : 0;
    out->ini_value = (ini_value >= 0 && ini_value < JPT_RENDERER_COUNT) ? ini_value : JPT_RENDERER_AUTO;

    if (env_text && env_text[0]) {
        out->env_present = 1;
        out->env_valid = jpt_renderer_parse(env_text, &out->env_value);
    }
    if (cli_text && cli_text[0]) {
        out->cli_present = 1;
        out->cli_valid = jpt_renderer_parse(cli_text, &out->cli_value);
    }

    if (out->cli_present && out->cli_valid) {
        out->requested = out->cli_value;
        out->source = JPT_RENDERER_SOURCE_CLI;
        if (out->env_present && out->env_valid) out->env_ignored = 1;
    } else if (out->ini_present && out->ini_value != JPT_RENDERER_AUTO) {
        out->requested = out->ini_value;
        out->source = JPT_RENDERER_SOURCE_INI;
        if (out->env_present && out->env_valid) out->env_ignored = 1;
    } else if (out->env_present && out->env_valid) {
        out->requested = out->env_value;
        out->source = JPT_RENDERER_SOURCE_ENV;
    } else if (out->ini_present) {
        out->requested = out->ini_value;
        out->source = JPT_RENDERER_SOURCE_INI;
    } else {
        out->requested = JPT_RENDERER_AUTO;
        out->source = JPT_RENDERER_SOURCE_DEFAULT;
    }
}

int jpt_renderer_parse(const char* text, jpt_renderer_t* out) {
    if (!text || !text[0]) return 0;
    for (int i = 0; i < JPT_RENDERER_COUNT; i++) {
        if (!_stricmp(text, k_keys[i])) {
            if (out) *out = (jpt_renderer_t)i;
            return 1;
        }
    }
    return 0;
}

int jpt_renderer_available_on_platform(jpt_renderer_t renderer, int is_windows) {
    if (renderer == JPT_RENDERER_D3D11) return is_windows ? 1 : 0;
    return renderer >= JPT_RENDERER_AUTO && renderer < JPT_RENDERER_COUNT;
}

int jpt_renderer_is_hardware(jpt_renderer_t renderer) { return renderer != JPT_RENDERER_SOFTWARE; }

void jpt_renderer_resolve_policy(jpt_renderer_t requested, int is_windows, int d3d11_available, int vulkan_available,
                                 const char* vulkan_unavailable_reason, jpt_renderer_decision_t* out) {
    const char* vk_why = (vulkan_unavailable_reason && vulkan_unavailable_reason[0]) ? vulkan_unavailable_reason
                                                                                     : "Vulkan device unavailable";
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->requested = requested;

    if (requested == JPT_RENDERER_SOFTWARE) {
        out->chosen = JPT_RENDERER_SOFTWARE;
        set_reason(out->reason, sizeof out->reason, "selected Software renderer");
        return;
    }

    if (requested == JPT_RENDERER_D3D11) {
        if (is_windows && d3d11_available) {
            out->chosen = JPT_RENDERER_D3D11;
            set_reason(out->reason, sizeof out->reason, "selected Direct3D 11 renderer");
            return;
        }
        out->fallback = 1;
        if (!is_windows) {
            if (vulkan_available) {
                out->chosen = JPT_RENDERER_VULKAN;
                set_reason(out->reason, sizeof out->reason,
                           "Direct3D 11 is unavailable on Linux; using Vulkan renderer");
            } else {
                out->chosen = JPT_RENDERER_SOFTWARE;
                snprintf(out->reason, sizeof out->reason,
                         "Direct3D 11 is unavailable on Linux and Vulkan failed (%s); falling back to Software",
                         vk_why);
            }
            return;
        }
        if (vulkan_available) {
            out->chosen = JPT_RENDERER_VULKAN;
            set_reason(out->reason, sizeof out->reason, "Direct3D 11 unavailable; falling back to Vulkan");
        } else {
            out->chosen = JPT_RENDERER_SOFTWARE;
            set_reason(out->reason, sizeof out->reason, "Direct3D 11 unavailable; falling back to Software");
        }
        return;
    }

    if (requested == JPT_RENDERER_VULKAN) {
        if (vulkan_available) {
            out->chosen = JPT_RENDERER_VULKAN;
            set_reason(out->reason, sizeof out->reason, "selected Vulkan renderer");
            return;
        }
        out->fallback = 1;
        out->chosen = JPT_RENDERER_SOFTWARE;
        snprintf(out->reason, sizeof out->reason, "Vulkan unavailable (%s); falling back to Software", vk_why);
        return;
    }

    /* Auto: preserve D3D11 as primary on Windows until Vulkan proves parity; prefer Vulkan on Linux. */
    if (is_windows) {
        if (d3d11_available) {
            out->chosen = JPT_RENDERER_D3D11;
            set_reason(out->reason, sizeof out->reason, "auto selected Direct3D 11 (Windows default)");
        } else if (vulkan_available) {
            out->chosen = JPT_RENDERER_VULKAN;
            out->fallback = 1;
            set_reason(out->reason, sizeof out->reason, "auto: Direct3D 11 unavailable; using Vulkan");
        } else {
            out->chosen = JPT_RENDERER_SOFTWARE;
            out->fallback = 1;
            set_reason(out->reason, sizeof out->reason, "auto: no hardware renderer available; falling back to Software");
        }
        return;
    }

    if (vulkan_available) {
        out->chosen = JPT_RENDERER_VULKAN;
        set_reason(out->reason, sizeof out->reason, "auto selected Vulkan (Linux default)");
    } else {
        out->chosen = JPT_RENDERER_SOFTWARE;
        out->fallback = 1;
        snprintf(out->reason, sizeof out->reason, "auto: Vulkan unavailable (%s); falling back to Software", vk_why);
    }
}

static int uuid_nonzero(const uint8_t uuid[16]) {
    if (!uuid) return 0;
    for (int i = 0; i < 16; i++)
        if (uuid[i] != 0) return 1;
    return 0;
}

void jpt_gpu_id_format(uint32_t vendor_id, uint32_t device_id, const uint8_t uuid[16], char* out, size_t cap) {
    if (!out || !cap) return;
    if (uuid_nonzero(uuid)) {
        snprintf(out, cap, "%04x:%04x:%02x%02x%02x%02x%02x%02x%02x%02x", vendor_id & 0xFFFFu, device_id & 0xFFFFu,
                 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7]);
    } else {
        snprintf(out, cap, "%04x:%04x", vendor_id & 0xFFFFu, device_id & 0xFFFFu);
    }
}

int jpt_gpu_request_is_auto(const char* request) {
    return !request || !request[0] || !_stricmp(request, "auto");
}

int jpt_gpu_match_request(const char* request, const char* label, const char* name, uint32_t vendor_id,
                          uint32_t device_id, const uint8_t uuid[16]) {
    char id[JPT_GPU_ID_MAX];
    char short_id[16];
    char full_uuid[33];
    if (jpt_gpu_request_is_auto(request)) return 0;
    if (label && label[0] && !strcmp(label, request)) return 1;
    jpt_gpu_id_format(vendor_id, device_id, uuid, id, sizeof id);
    if (!_stricmp(id, request)) return 1;
    snprintf(short_id, sizeof short_id, "%04x:%04x", vendor_id & 0xFFFFu, device_id & 0xFFFFu);
    if (!_stricmp(short_id, request)) return 1;
    if (uuid_nonzero(uuid)) {
        for (int i = 0; i < 16; i++) snprintf(full_uuid + i * 2, 3, "%02x", uuid[i]);
        full_uuid[32] = 0;
        if (!_stricmp(full_uuid, request)) return 1;
    }
    if (name && name[0] && !_stricmp(name, request)) return 1;
    return 0;
}
