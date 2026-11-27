#include "jpt_szdd.h"

#include "jpt_alloc.h"

#include <stdlib.h>
#include <string.h>

#define WINDOW 4096
#define MOST_EXPANDED 9 /* 16 matches of 18 bytes from 2 each, and their control bytes */

int jpt_szdd_is(const uint8_t* in, size_t in_size) {
    static const uint8_t magic[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };
    return in_size >= JPT_SZDD_HEADER && !memcmp(in, magic, sizeof magic) && in[8] == 'A';
}

/* LZSS: a 4096-byte window of spaces, written from 4096 - 16; a control byte tells literals (1) from matches (0), a match
 * is a 12-bit window position and a 4-bit length - 3. */
uint8_t* jpt_szdd_expand(const uint8_t* in, size_t in_size, size_t* out_size) {
    uint8_t window[WINDOW];
    uint8_t* out;
    size_t want, o = 0, i = JPT_SZDD_HEADER;
    int w = WINDOW - 16;
    if (!jpt_szdd_is(in, in_size)) return NULL;
    want = (size_t)in[10] | (size_t)in[11] << 8 | (size_t)in[12] << 16 | (size_t)in[13] << 24;
    if (want / MOST_EXPANDED > in_size) return NULL;
    out = (uint8_t*)jpt_alloc(want, 1);
    if (!out) return NULL;
    memset(window, ' ', sizeof window);
    while (o < want && i < in_size) {
        unsigned control = in[i++];
        for (int bit = 0; bit < 8 && o < want; bit++, control >>= 1) {
            if (control & 1) {
                if (i >= in_size) break;
                out[o++] = window[w] = in[i++];
                w = (w + 1) & (WINDOW - 1);
            } else {
                int pos, len;
                if (i + 1 >= in_size) break;
                pos = in[i] | ((in[i + 1] & 0xF0) << 4);
                len = (in[i + 1] & 0x0F) + 3;
                i += 2;
                for (int k = 0; k < len && o < want; k++) {
                    uint8_t c = window[(pos + k) & (WINDOW - 1)];
                    out[o++] = window[w] = c;
                    w = (w + 1) & (WINDOW - 1);
                }
            }
        }
    }
    *out_size = o;
    return out;
}
