/* Button prompts as vector shapes: what a gamepad control or a key looks like, and its anti-aliased coverage at any
 * size, so prompts stay sharp at every resolution without fonts that hold controller symbols. */
#ifndef PAD_GLYPHS_H
#define PAD_GLYPHS_H

#include <stdint.h>

#include "jpt_gamepad.h"
#include "jpt_prompt_text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAD_GLYPH_FACE,        /* a round button with a letter or a PlayStation symbol */
    PAD_GLYPH_DIAMOND,     /* a generic face button: four positions, one lit */
    PAD_GLYPH_BUMPER,
    PAD_GLYPH_TRIGGER,
    PAD_GLYPH_PILL,        /* View, Menu, Options... */
    PAD_GLYPH_DPAD,        /* a cross with one arm lit */
    PAD_GLYPH_STICK,       /* a stick, its cap tilted towards a direction */
    PAD_GLYPH_STICK_CLICK,
    PAD_GLYPH_KEY,
    PAD_GLYPH_MOUSE        /* a mouse with one button lit */
} pad_glyph_shape_t;

typedef enum { PAD_SYMBOL_NONE, PAD_SYMBOL_CROSS, PAD_SYMBOL_CIRCLE, PAD_SYMBOL_SQUARE, PAD_SYMBOL_TRIANGLE } pad_symbol_t;

/* Layers drawn in order: body, rim, marks (letters' colour), accent. */
enum { PAD_LAYER_BODY, PAD_LAYER_RIM, PAD_LAYER_MARK, PAD_LAYER_ACCENT, PAD_LAYER_COUNT };

typedef struct {
    uint8_t shape, symbol;
    uint8_t dir;         /* JPT_DIR_* for sticks and the D-pad; the lit position of a diamond (0 south..3 north); the
                            mouse button (1 left, 2 right, 3 middle) */
    uint8_t tilt;        /* a stick's travel: 0 centred, 1 half, 2 full */
    float width;         /* the glyph's width in units of its height */
    float text_x, text_y;/* the text's centre, in height units from the glyph's corner */
    float text_size;     /* the text's height in height units */
    uint32_t color[PAD_LAYER_COUNT]; /* 0xRRGGBB per layer */
    uint32_t text_color;
    char text[16];
} pad_glyph_t;

/* 0 when the control has no glyph. */
int pad_glyph_control(jpt_pad_family_t family, int control, int dir, int tilt, pad_glyph_t* out);
/* A key with its label (jpt_key_label); mouse buttons become a mouse. */
int pad_glyph_key(uint32_t vk, const char* label, pad_glyph_t* out);
/* A prompt token's glyph in a family's style: a key, a mouse, or a gamepad control. */
int pad_glyph_token(const jpt_prompt_token_t* token, jpt_pad_family_t family, pad_glyph_t* out);
/* Coverage 0..1 of a layer at (u, v) in height units from the glyph's corner; aa is the size of a pixel there. */
float pad_glyph_coverage(const pad_glyph_t* g, int layer, float u, float v, float aa);

#ifdef __cplusplus
}
#endif

#endif
