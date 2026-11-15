#include "text_layout.h"

#include <string.h>

static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int is_break(unsigned char c) { return c == ' ' || c == ',' || c == ';' || c == ':' || c == '-'; }

size_t tl_layout(const char* text, uint32_t flags, const tl_view_t* view, tl_width_fn width, void* ctx, uint8_t* out,
                 size_t cap) {
    const unsigned char* in = (const unsigned char*)text;
    const uint32_t max_w = (uint32_t)(view->width - 0x10);
    size_t records[TL_MAX_LINES], o = 8;
    int32_t y = 8;
    int lines = 0;
    if (cap < 8) return 0;
    while (*in && lines < TL_MAX_LINES) {
        const unsigned char *start = in, *break_in = NULL;
        size_t rec = o, break_out = 0;
        uint32_t count = 0, sum = 0, break_count = 0, break_sum = 0;
        int after_break = 0;
        unsigned char c = *in;
        if (o + TL_RECORD + 1 > cap) return 0;
        o += TL_RECORD;
        while (c) {
            uint32_t next;
            if (c == '\r') {
                in++;
                break;
            }
            next = sum + width(ctx, c);
            if (next >= max_w) break;
            if (o + 2 > cap) return 0;
            count++;
            sum = next;
            out[o] = c;
            after_break = is_break(c);
            if (after_break) {
                break_in = in + 1;
                break_out = o + 1;
                break_sum = sum;
                break_count = count;
            }
            o++;
            c = *++in;
        }
        if (!after_break && break_out && *in) {
            in = break_in;
            count = break_count;
            sum = break_sum;
            out[break_out] = 0;
            o = break_out + 1;
        } else {
            out[o++] = 0;
        }
        /* A glyph wider than the line: retail would lay out empty lines forever. */
        if (in == start && *in) {
            in++;
            if (o + 1 > cap) return 0;
        }
        if (flags & TL_ALIGN_LEFT)
            put32(out + rec, (uint32_t)(view->left + 8));
        else if (flags & TL_ALIGN_CENTRE)
            put32(out + rec, (uint32_t)(((int32_t)max_w - (int32_t)sum) / 2 + view->left + 8));
        else if (flags & TL_ALIGN_RIGHT)
            put32(out + rec, (uint32_t)(view->left + view->width - (int32_t)sum - 8));
        put32(out + rec + 8, count);
        put32(out + rec + 4, (uint32_t)(view->top + y));
        records[lines++] = rec;
        y += view->line_height;
    }
    if ((flags & TL_ALIGN_BOTTOM) && lines > 0)
        for (int i = 0; i < lines; i++)
            put32(out + records[i] + 4, get32(out + records[i] + 4) + (uint32_t)(view->height - y - 8));
    put32(out + 4, (uint32_t)lines);
    put32(out, (uint32_t)o);
    return o;
}
