#include "overlay_text.h"

#include "compass_policy.h"
#include "guest_abi.h"

#define OVERLAY_PTR GVA(0x0066F03C)
#define RASTER_PTR GVA(0x0073DC40)
#define RASTER_HEIGHT 0x0Cu
#define RASTER_TOP 0x8Cu
/* CTextOverlay: its entries, a ring from the sentinel at +0x54; an entry's laid-out text, flags and category. */
#define OVERLAY_LIST 0x54u
#define NODE_BLOB 0x0Cu
#define NODE_FLAGS 0x1Cu
#define NODE_CATEGORY 0x24u
#define NODE_WAITING 0x400u
#define NODE_LIMIT 64
#define MAX_LINES 32

int overlay_blob_lines(uint32_t blob, overlay_line_t* lines, int max) {
    uint32_t size, count, p, end;
    if (!blob) return -1;
    size = MEM32(blob);
    count = MEM32(blob + 4);
    if (size < 8 || size > OVERLAY_BLOB_MAX || count > (uint32_t)max) return -1;
    end = blob + size;
    p = blob + 8;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 12 > end) return -1;
        lines[i].x = (int)MEM32(p);
        lines[i].y = (int)MEM32(p + 4);
        lines[i].count = (int)MEM32(p + 8);
        lines[i].text = p + 12;
        if (lines[i].count < 0 || p + 12 + (uint32_t)lines[i].count + 1 > end) return -1;
        p += 12 + (uint32_t)lines[i].count + 1;
    }
    return (int)count;
}

int overlay_blob_readable(uint32_t blob) {
    uint32_t size;
    return blob && guest_read32(blob, &size) && size <= OVERLAY_BLOB_MAX && guest_readable(blob, size);
}

/* The picture's top and height in the raster the text is laid out in; a huge height (everything counts as top text)
 * when the raster cannot be read. */
static void text_raster(int* top, int* height) {
    uint32_t raster = 0, t = 0, h = 0;
    *top = 0;
    *height = 1 << 30;
    if (!guest_read32(RASTER_PTR, &raster) || !raster || !guest_read32(raster + RASTER_TOP, &t) ||
        !guest_read32(raster + RASTER_HEIGHT, &h) || !h)
        return;
    *top = (int)t;
    *height = (int)h;
}

void overlay_text_state(int* card, int* game_text) {
    uint32_t overlay = 0, head = 0, node = 0;
    int n = 0, raster_top, raster_h;
    *card = *game_text = 0;
    text_raster(&raster_top, &raster_h);
    if (!guest_read32(OVERLAY_PTR, &overlay) || !overlay || !guest_read32(overlay + OVERLAY_LIST, &head) || !head ||
        !guest_read32(head, &node))
        return;
    for (; node && node != head && n < NODE_LIMIT; node = MEM32(node), n++) {
        overlay_line_t lines[MAX_LINES];
        uint32_t blob;
        if (!guest_readable(node, NODE_CATEGORY + 4)) return;
        if (MEM32(node + NODE_FLAGS) & NODE_WAITING) continue;
        if (MEM32(node + NODE_CATEGORY) == OVERLAY_CARD_CATEGORY) {
            *card = 1;
            continue;
        }
        blob = MEM32(node + NODE_BLOB);
        if (!overlay_blob_readable(blob) || overlay_blob_lines(blob, lines, MAX_LINES) <= 0) continue;
        if (cp_text_hides_hud(cp_text_kind(MEM32(node + NODE_CATEGORY), MEM32(node + NODE_CATEGORY) == OVERLAY_HINT_CATEGORY,
                                           lines[0].y - raster_top, raster_h)))
            *game_text = 1;
    }
}
