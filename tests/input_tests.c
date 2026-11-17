/*
 * The mouse and the gamepad through the game's own CInput::tinReadDefaultControls (0x005F80B0, the lifted code the
 * game runs), one game frame at a time, with the host's per-frame steps around it as gamepad.c and window_host.c run
 * them: the pad's translation before and after the read, and every relative motion into the one virtual cursor. The
 * left mouse button and LT, mouse counts and the right stick must give CPlayer the same SInput, frame for frame.
 * Headless: ninja -C <build dir> input_tests && input_tests.exe
 * The guest addresses the code uses live in g_window, which the fixed image base puts there.
 */
#include <windows.h>
#include "test_profile.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gamepad_policy.h"
#include "host_policy.h"
#include "recomp_types.h"

void lifted_sub_005F80B0(void);

uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double g_st[8];
int g_fp_top;
#ifdef RECOMP_X87_RING
double g_stbuf[X87_RING], *g_stp = g_stbuf + X87_RING / 2;
int64_t g_sti64buf[X87_RING], *g_sti64p = g_sti64buf + X87_RING / 2;
void recomp_x87_recentre(void) {
    double v[8];
    int64_t w[8];
    memcpy(v, g_stp, sizeof v), memcpy(w, g_sti64p, sizeof w);
    g_stp = g_stbuf + X87_RING / 2, g_sti64p = g_sti64buf + X87_RING / 2;
    memcpy(g_stp, v, sizeof v), memcpy(g_sti64p, w, sizeof w);
}
#endif
uint16_t g_fpu_cw = 0x027F;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_icall_count;

/* The game's imports CInput calls through its IAT, and the data it reads. */
#define IAT_GET_CURSOR_POS 0x00645360u
#define IAT_GET_ASYNC_KEY_STATE 0x006453D4u
#define IAT_GET_CLIENT_RECT 0x006453D8u
#define IAT_CLIENT_TO_SCREEN 0x006453DCu
#define IAT_SET_CURSOR_POS 0x006453E0u
#define KEY_MAP 0x0066AE78u
#define KEY_MAP_END 0x0066AEF0u
#define RADIANS_PER_COUNT 0x0064E82Cu
#define INVERT_FACTOR 0x00647608u
#define INVERT_MOUSE 0x006603D8u
#define FAKE_GET_CURSOR_POS 0x7E000010u
#define FAKE_GET_ASYNC_KEY_STATE 0x7E000020u
#define FAKE_GET_CLIENT_RECT 0x7E000030u
#define FAKE_CLIENT_TO_SCREEN 0x7E000040u
#define FAKE_SET_CURSOR_POS 0x7E000050u
#define WINDOW_LO 0x00645000u
#define WINDOW_HI 0x00800000u
#define CINPUT 0x00700000u
#define STACK_TOP 0x007EF000u
#define CLIENT_W 1024
#define CLIENT_H 768
/* CInput: the window, SInput (+8: time, rotate x/y, move x/y, state, hit), the keys' own last state, the last centre. */
#define CI_ROTATE_X 0x0Cu
#define CI_ROTATE_Y 0x10u
#define CI_MOVE_X 0x14u
#define CI_MOVE_Y 0x18u
#define CI_STATE 0x1Cu
#define CI_HIT 0x20u
#define CI_PREVIOUS 0x24u
#define CI_CENTRE_X 0x28u
#define CI_CENTRE_Y 0x2Cu
#define SINPUT_IN_CINPUT 0x08u

static uint8_t g_window[0x600000];
static unsigned long g_checks, g_failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAILED line %d: %s\n", __LINE__, #c);                                                              \
        }                                                                                                              \
    } while (0)

static void put32(uint32_t va, uint32_t v) { memcpy((void*)(uintptr_t)va, &v, 4); }
static uint32_t get32(uint32_t va) {
    uint32_t v;
    memcpy(&v, (const void*)(uintptr_t)va, 4);
    return v;
}
static float getf(uint32_t va) {
    float v;
    memcpy(&v, (const void*)(uintptr_t)va, 4);
    return v;
}
static void putf(uint32_t va, float v) { memcpy((void*)(uintptr_t)va, &v, 4); }

/* The host: the one virtual cursor in gameplay, and what Windows reports for the mouse's buttons. */
static struct {
    input_state_t state;
    long cursor_x, cursor_y, read_x, read_y;
    long real_x, real_y; /* the operating system's cursor, in display pixels */
    int host_flips;      /* the state follows the game's stoppage, as update_input does */
    int stopped;
    int recapture, update_after_hooks; /* the host's two guards against a stale state */
    uint8_t down[256], since[256];
    float sensitivity; /* Mouse sensitivity, applied to the mouse's counts as mouse_relative does */
    mouse_scale_t scale;
} H;

/* update_input: the state from what the game shows, the virtual cursor re-set when the game takes its pointer back. */
static void host_update(void) {
    input_signals_t signals;
    input_state_t next;
    if (!H.host_flips) return;
    memset(&signals, 0, sizeof signals);
    signals.focused = 1;
    signals.ui = H.stopped ? GUEST_UI_DIALOG : GUEST_UI_GAMEPLAY;
    next = input_state_from(&signals);
    if (next == H.state) return;
    if (H.recapture) pointer_recapture(H.state, next, &H.cursor_x, &H.cursor_y, CLIENT_W, CLIENT_H);
    H.state = next;
}

/* game_cursor(): while the cursor is not virtual the game reads the real one, and the virtual cursor takes it. */
static void host_read_cursor(void) {
    if (input_cursor_virtual(H.state)) return;
    H.cursor_x = H.real_x;
    H.cursor_y = H.real_y;
}

static void stdcall_return(uint32_t value, int args) {
    g_eax = value;
    g_esp += 4 + 4 * (uint32_t)args;
}

/* shim_GetCursorPos in gameplay: the virtual cursor. */
static void stub_get_cursor_pos(void) {
    uint32_t out = get32(g_esp + 4);
    host_update();
    host_read_cursor();
    H.read_x = H.cursor_x;
    H.read_y = H.cursor_y;
    put32(out, (uint32_t)H.cursor_x);
    put32(out + 4, (uint32_t)H.cursor_y);
    stdcall_return(1, 1);
}

/* GetAsyncKeyState as Windows answers it: bit 15 down, bit 0 pressed since the last call. */
static void stub_get_async_key_state(void) {
    uint32_t vk = get32(g_esp + 4) & 0xFF;
    uint32_t v = (H.down[vk] ? 0x8000u : 0u) | (H.since[vk] ? 1u : 0u);
    H.since[vk] = 0;
    stdcall_return(v, 1);
}

/* shim_GetClientRect / shim_ClientToScreen on the virtual window: the game's screen, already in screen terms. */
static void stub_get_client_rect(void) {
    uint32_t r = get32(g_esp + 8);
    put32(r, 0), put32(r + 4, 0), put32(r + 8, CLIENT_W), put32(r + 12, CLIENT_H);
    stdcall_return(1, 2);
}

static void stub_client_to_screen(void) { stdcall_return(1, 2); }

/* shim_SetCursorPos: the virtual cursor only. */
static void stub_set_cursor_pos(void) {
    H.cursor_x = (long)(int32_t)get32(g_esp + 4);
    H.cursor_y = (long)(int32_t)get32(g_esp + 8);
    stdcall_return(1, 2);
}

recomp_func_t recomp_lookup(uint32_t va) {
    switch (va) {
    case FAKE_GET_CURSOR_POS: return stub_get_cursor_pos;
    case FAKE_GET_ASYNC_KEY_STATE: return stub_get_async_key_state;
    case FAKE_GET_CLIENT_RECT: return stub_get_client_rect;
    case FAKE_CLIENT_TO_SCREEN: return stub_client_to_screen;
    case FAKE_SET_CURSOR_POS: return stub_set_cursor_pos;
    }
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}
recomp_func_t recomp_lookup_import(uint32_t va) {
    (void)va;
    return NULL;
}

/* The game's key map and constants from its executable (the key map as shipped: the registry's Controls entries
 * change it the same way for both devices). */
static int read_exe(uint32_t va, void* out, uint32_t n) {
    FILE* f = fopen(INPUT_GAME_EXE, "rb");
    uint8_t head[0x400];
    uint32_t pe, sections, optional;
    int ok = 0;
    if (!f) return 0;
    if (fread(head, 1, sizeof head, f) == sizeof head) {
        memcpy(&pe, head + 0x3C, 4);
        sections = *(uint16_t*)(head + pe + 6);
        optional = *(uint16_t*)(head + pe + 20);
        for (uint32_t i = 0; i < sections && pe + 24 + optional + 40 * (i + 1) <= sizeof head && !ok; i++) {
            const uint8_t* s = head + pe + 24 + optional + 40 * i;
            uint32_t sva, raw_size, raw;
            memcpy(&sva, s + 12, 4);
            memcpy(&raw_size, s + 16, 4);
            memcpy(&raw, s + 20, 4);
            sva += 0x00400000u;
            if (va < sva || va + n > sva + raw_size) continue;
            ok = !fseek(f, (long)(raw + (va - sva)), SEEK_SET) && fread(out, 1, n, f) == n;
        }
    }
    fclose(f);
    return ok;
}

static int load_game_data(void) {
    /* The same bytes, as read from trespass.exe (sha256 d88bb548...), for a tree without the game. */
    static const uint32_t key_map[30] = { 0x01, 0x40, 0x02, 0x80, 0x10, 0x10, 0x11, 0x08, 0x20, 0x20, 0x51, 0x01,
                                          0x5A, 0x02, 0x45, 0x100, 0x46, 0x400, 0x52, 0x200, 0x57, 0x01000000,
                                          0x53, 0x02000000, 0x58, 0x04000000, 0x41, 0x08000000, 0x44, 0x10000000 };
    int from_exe = read_exe(KEY_MAP, (void*)(uintptr_t)KEY_MAP, KEY_MAP_END - KEY_MAP) &&
                   read_exe(RADIANS_PER_COUNT, (void*)(uintptr_t)RADIANS_PER_COUNT, 4) &&
                   read_exe(INVERT_FACTOR, (void*)(uintptr_t)INVERT_FACTOR, 4);
    if (!from_exe) {
        memcpy((void*)(uintptr_t)KEY_MAP, key_map, sizeof key_map);
        putf(RADIANS_PER_COUNT, 0.005f);
        putf(INVERT_FACTOR, -1.0f);
    }
    put32(IAT_GET_CURSOR_POS, FAKE_GET_CURSOR_POS);
    put32(IAT_GET_ASYNC_KEY_STATE, FAKE_GET_ASYNC_KEY_STATE);
    put32(IAT_GET_CLIENT_RECT, FAKE_GET_CLIENT_RECT);
    put32(IAT_CLIENT_TO_SCREEN, FAKE_CLIENT_TO_SCREEN);
    put32(IAT_SET_CURSOR_POS, FAKE_SET_CURSOR_POS);
    return from_exe;
}

/* ---- one game frame ---- */

typedef struct {
    int hand;          /* the left mouse button, or LT, held at this read */
    long count_x;      /* mouse: Raw Input counts since the last read */
    long count_y;
    float stick_x;     /* gamepad: the right stick (-1..1, up negative) */
    float stick_y;
    float shoulder;    /* gamepad: LB held (Rotate Wrist), as Shift with the mouse */
    float dt;          /* seconds since the last read */
    int away;          /* a menu or video ran since the last read */
} step_t;

/* What the read gave the game (SInput as CPlayer's message carries it) and the input state around it. */
typedef struct {
    uint32_t rotate_x, rotate_y, move_x, move_y; /* the floats' bits */
    uint32_t state, hit;
    int previous_hand;                    /* Move Hand in the state the next read's press is decided against */
    long read_x, read_y, after_x, after_y; /* the cursor CInput read, and where it left it */
    uint32_t centre_x, centre_y;
} frame_t;

typedef struct {
    jpt_gamepad_settings_t cfg;
    pad_play_t play;
    uint8_t triggers[2];
    int pads; /* the gamepad drives this run; otherwise the mouse and keyboard */
    int started;
} rig_t;

static void rig_init(rig_t* r, int pads, int invert_mouse) {
    memset(r, 0, sizeof *r);
    memset(&H, 0, sizeof H);
    H.sensitivity = 1.0f;
    jpt_gamepad_defaults(&r->cfg);
    r->pads = pads;
    H.state = INPUT_GAMEPLAY_CAPTURED;
    *(uint8_t*)(uintptr_t)INVERT_MOUSE = (uint8_t)invert_mouse;
    /* CInput's constructor (0x005F7FB0): no keys yet, the cursor at the centre. */
    memset((void*)(uintptr_t)CINPUT, 0, 0x40);
    put32(CINPUT, 0x00010001u);
    put32(CINPUT + CI_CENTRE_X, CLIENT_W / 2);
    put32(CINPUT + CI_CENTRE_Y, CLIENT_H / 2);
    H.cursor_x = CLIENT_W / 2;
    H.cursor_y = CLIENT_H / 2;
}

static void lifted_read(void) {
    put32(STACK_TOP, RECOMP_RETADDR);
    g_esp = STACK_TOP;
    g_ecx = CINPUT;
    g_alias_entry = 0;
    lifted_sub_005F80B0();
    CHECK(g_esp == STACK_TOP + 4 && g_eax == CINPUT + SINPUT_IN_CINPUT);
}

static frame_t run_step(rig_t* r, const step_t* s) {
    pad_state_t pad;
    pad_sinput_t in;
    frame_t f;
    int dx = 0, dy = 0;
    memset(&pad, 0, sizeof pad);
    /* Between reads: the mouse's buttons and Raw Input, or the controller's state. */
    if (!r->pads) {
        if (s->hand && !H.down[VK_LBUTTON]) H.since[VK_LBUTTON] = 1;
        H.down[VK_LBUTTON] = (uint8_t)(s->hand != 0);
        H.down[VK_SHIFT] = (uint8_t)(s->shoulder != 0.0f);
        long x = s->count_x, y = s->count_y;
        if (input_captured(H.state)) mouse_scale_counts(&H.scale, H.sensitivity, s->count_x, s->count_y, &x, &y);
        pointer_add_relative(H.state, &H.cursor_x, &H.cursor_y, x, y);
    } else {
        pad.lt = s->hand ? 1.0f : 0.0f;
        pad.rx = s->stick_x;
        pad.ry = s->stick_y;
        if (s->shoulder != 0.0f) pad.buttons |= JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER);
    }
    /* gamepad.c's cinput_read: the pad before the read (idle with the mouse), the read, the pad after it. */
    pad_play_before(&r->play, pad_read_kind(!r->started, s->away, 1, s->dt), &pad,
                    pad_buttons_with_trigger_hold(&pad, r->cfg.trigger_deadzone / 100.0f, r->triggers), &r->cfg, s->dt,
                    *(uint8_t*)(uintptr_t)INVERT_MOUSE != 0, &dx, &dy);
    r->started = 1;
    pointer_add_relative(H.state, &H.cursor_x, &H.cursor_y, dx, dy);
    lifted_read();
    in.move_x = getf(CINPUT + CI_MOVE_X);
    in.move_y = getf(CINPUT + CI_MOVE_Y);
    in.state = get32(CINPUT + CI_STATE);
    in.hit = get32(CINPUT + CI_HIT);
    if (pad_play_after(&r->play, &r->cfg, &in)) {
        putf(CINPUT + CI_MOVE_X, in.move_x);
        putf(CINPUT + CI_MOVE_Y, in.move_y);
        put32(CINPUT + CI_STATE, in.state);
        put32(CINPUT + CI_HIT, in.hit);
    }
    memset(&f, 0, sizeof f);
    f.rotate_x = get32(CINPUT + CI_ROTATE_X);
    f.rotate_y = get32(CINPUT + CI_ROTATE_Y);
    f.move_x = get32(CINPUT + CI_MOVE_X);
    f.move_y = get32(CINPUT + CI_MOVE_Y);
    f.state = get32(CINPUT + CI_STATE);
    f.hit = get32(CINPUT + CI_HIT);
    f.previous_hand = r->pads ? (pad_commands_for(&r->cfg, r->play.edges.previous) & JPT_CMD_HAND) != 0
                              : (get32(CINPUT + CI_PREVIOUS) & JPT_CMD_HAND) != 0;
    f.read_x = H.read_x, f.read_y = H.read_y;
    f.after_x = H.cursor_x, f.after_y = H.cursor_y;
    f.centre_x = get32(CINPUT + CI_CENTRE_X), f.centre_y = get32(CINPUT + CI_CENTRE_Y);
    return f;
}

/* The stick held for dt gives (int)(rate * dt / 0.005) counts; these dt give 20.5 and 15.5 counts at full stick, so
 * whole counts of 20 and 15 with the half carried and dropped as the stick centres. */
#define DT_X20 (20.5f * 0.005f / PAD_LOOK_YAW_RATE)
#define DT_Y15 (15.5f * 0.005f / PAD_LOOK_PITCH_RATE)
#define DT_FRAME (1.0f / 60.0f)

static const char* field_differs(const frame_t* a, const frame_t* b) {
    if (a->state != b->state) return "state";
    if (a->hit != b->hit) return "hit";
    if (a->previous_hand != b->previous_hand) return "previous Move Hand";
    if (a->rotate_x != b->rotate_x || a->rotate_y != b->rotate_y) return "rotate";
    if (a->move_x != b->move_x || a->move_y != b->move_y) return "move";
    if (a->read_x != b->read_x || a->read_y != b->read_y) return "cursor read";
    if (a->after_x != b->after_x || a->after_y != b->after_y) return "cursor after";
    if (a->centre_x != b->centre_x || a->centre_y != b->centre_y) return "centre";
    return NULL;
}

/* Runs the steps with the mouse and with the gamepad and compares every frame. With InvertMouse on the game turns the
 * mouse's vertical round but not the stick's (it has its own Invert Y): the stick is pushed the other way for the same
 * look. */
static int compare_runs(const char* name, const step_t* steps, int n, int invert_mouse, frame_t* mouse_out) {
    rig_t mouse, pad;
    int same = 1;
    float sign_y = invert_mouse ? -1.0f : 1.0f;
    rig_init(&mouse, 0, invert_mouse);
    for (int i = 0; i < n; i++) mouse_out[i] = run_step(&mouse, &steps[i]);
    rig_init(&pad, 1, invert_mouse);
    for (int i = 0; i < n; i++) {
        step_t s = steps[i];
        frame_t p;
        const char* d;
        s.stick_y *= sign_y;
        p = run_step(&pad, &s);
        d = field_differs(&mouse_out[i], &p);
        if (d) {
            same = 0;
            printf("  %s, frame %d: %s differs (mouse state %08X hit %08X rotate %08X/%08X, gamepad state %08X hit %08X "
                   "rotate %08X/%08X)\n",
                   name, i, d, mouse_out[i].state, mouse_out[i].hit, mouse_out[i].rotate_x, mouse_out[i].rotate_y,
                   p.state, p.hit, p.rotate_x, p.rotate_y);
        }
    }
    return same;
}

static float rot(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void test_sequences(void) {
    /* The reference: Move Hand down, +20 X, stop, +20 X, stop, -20 X, stop, Move Hand up; then up and down 15, a frame
     * of 0.4 s (a load, a hitch) with the hand held and moving, the wrist with the hand, a re-press. */
    static const step_t seq[] = {
        { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* idle */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* Move Hand down */
        { 1, 20, 0, 1.0f, 0.0f, 0.0f, DT_X20, 0 },      /* +20 X */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* stop */
        { 1, 20, 0, 1.0f, 0.0f, 0.0f, DT_X20, 0 },      /* +20 X */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* stop */
        { 1, -20, 0, -1.0f, 0.0f, 0.0f, DT_X20, 0 },    /* -20 X */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* stop */
        { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* Move Hand up */
        { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 1, 0, -15, 0.0f, -1.0f, 0.0f, DT_Y15, 0 },    /* pressed with 15 up (mouse forward, stick up) */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 1, 0, 15, 0.0f, 1.0f, 0.0f, DT_Y15, 0 },      /* down 15 */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, 0.4f, 0 },         /* a 0.4 s frame, held */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 1, 20, 0, 1.0f, 0.0f, 0.0f, DT_X20, 0 },      /* still the hand's motion after it */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, 0.05f, 1 },        /* a menu opened and closed, held throughout */
        { 1, 0, 0, 0.0f, 0.0f, 1.0f, DT_FRAME, 0 },     /* wrist (Shift / LB) with the hand */
        { 1, 20, 0, 1.0f, 0.0f, 1.0f, DT_X20, 0 },
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 0, 0, 0, 0.0f, 0.0f, 1.0f, DT_FRAME, 0 },     /* the wrist alone: no hand, as Shift alone */
        { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },     /* pressed again */
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
    };
    enum { N = sizeof seq / sizeof seq[0] };
    frame_t m[N];
    int same;
    same = compare_runs("InvertMouse off", seq, N, 0, m);
    CHECK(same);
    printf("mouse vs gamepad through the lifted CInput, InvertMouse off: %d frames %s\n", N,
           same ? "identical (state, hit, previous Move Hand, rotate, move, cursor read/after, centre)" : "DIFFERENT");
    /* what those frames are, so the comparison is not two equal mistakes */
    CHECK(m[0].state == 0 && m[0].hit == 0 && !m[0].previous_hand);
    CHECK(m[1].state == JPT_CMD_HAND && m[1].hit == JPT_CMD_HAND && m[1].previous_hand);  /* edge down */
    CHECK(m[1].rotate_x == 0 && m[1].rotate_y == 0);
    CHECK(m[2].state == JPT_CMD_HAND && m[2].hit == 0 && m[2].previous_hand);              /* held, no edge */
    CHECK(m[2].read_x == CLIENT_W / 2 + 20 && m[2].read_y == CLIENT_H / 2);               /* the cursor CInput read */
    CHECK(m[2].after_x == CLIENT_W / 2 && m[2].after_y == CLIENT_H / 2);                  /* re-centred */
    CHECK(fabsf(rot(m[2].rotate_x) - 0.1f) < 1e-6f && m[2].rotate_y == 0);               /* 20 x 0.005 rad */
    CHECK(m[3].rotate_x == 0 && m[3].state == JPT_CMD_HAND);
    CHECK(fabsf(rot(m[6].rotate_x) + 0.1f) < 1e-6f);
    CHECK(m[8].state == 0 && m[8].hit == 0 && !m[8].previous_hand);                        /* edge up */
    CHECK(fabsf(rot(m[10].rotate_y) - 0.075f) < 1e-6f && m[10].hit == JPT_CMD_HAND);      /* up: positive */
    CHECK(fabsf(rot(m[12].rotate_y) + 0.075f) < 1e-6f);
    CHECK(m[13].state == JPT_CMD_HAND && m[14].state == JPT_CMD_HAND && m[13].hit == 0);   /* through the long frame */
    CHECK(m[15].state == JPT_CMD_HAND && fabsf(rot(m[15].rotate_x) - 0.1f) < 1e-6f);
    CHECK(m[16].state == JPT_CMD_HAND && m[16].hit == 0);                                  /* and the menu */
    CHECK(m[17].state == (JPT_CMD_HAND | JPT_CMD_WRIST) && m[17].hit == JPT_CMD_WRIST);
    CHECK(m[20].state == JPT_CMD_WRIST);
    CHECK(m[22].state == JPT_CMD_HAND && m[22].hit == JPT_CMD_HAND);
    same = compare_runs("InvertMouse on", seq, N, 1, m);
    CHECK(same);
    printf("mouse vs gamepad through the lifted CInput, InvertMouse on: %d frames %s\n", N,
           same ? "identical" : "DIFFERENT");
}

/* The pause rules: held buttons are held after a long frame or a menu, as the left button is; presses made away from
 * gameplay (the menu's A or B) wait for their release. */
static void test_pauses(void) {
    static const step_t long_frame[] = {
        { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, 0.6f, 0 },
        { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
    };
    rig_t r;
    frame_t f;
    pad_edges_t old;
    uint32_t pressed, lt = JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER);
    rig_init(&r, 1, 0);
    run_step(&r, &long_frame[0]);
    for (int i = 1; i < 4; i++) {
        f = run_step(&r, &long_frame[i]);
        CHECK(f.state == JPT_CMD_HAND && f.hit == (i == 1 ? JPT_CMD_HAND : 0u));
    }
    /* before this round a frame past 0.25 s reset the edges and held LT counted as a fresh press to wait out: Move
     * Hand dropped until LT was let go and pressed again, while the left button stays held */
    memset(&old, 0, sizeof old);
    pad_edges_update(&old, lt, &pressed);
    pad_edges_reset(&old, lt);
    CHECK(pad_edges_update(&old, lt, &pressed) == 0);
    {
        /* LT first pressed while a menu was open: as the menu's own buttons, it counts once released and pressed */
        static const step_t menu_press[] = {
            { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
            { 1, 0, 0, 0.0f, 0.0f, 0.0f, 0.05f, 1 },
            { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
            { 1, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 },
        };
        rig_init(&r, 1, 0);
        f = run_step(&r, &menu_press[0]);
        f = run_step(&r, &menu_press[1]);
        CHECK(f.state == 0 && f.hit == 0);
        f = run_step(&r, &menu_press[2]);
        f = run_step(&r, &menu_press[3]);
        CHECK(f.state == JPT_CMD_HAND && f.hit == JPT_CMD_HAND);
    }
    {
        /* a released pointer (Alt+Tab, a dialog) takes no motion from either device */
        rig_t m;
        step_t s = { 1, 30, 0, 1.0f, 0.0f, 0.0f, DT_X20, 0 };
        rig_init(&m, 0, 0);
        H.state = INPUT_GAMEPLAY_RELEASED;
        f = run_step(&m, &s);
        CHECK(f.rotate_x == 0 && f.read_x == CLIENT_W / 2);
        rig_init(&r, 1, 0);
        H.state = INPUT_GAMEPLAY_RELEASED;
        f = run_step(&r, &s);
        CHECK(f.rotate_x == 0 && f.read_x == CLIENT_W / 2);
    }
}

/* F7 in gameplay, as window_host.c and the game run it: the UI loop's GetCursorPos calls the hooks, which pause the game
 * (SetCursorPos to the centre), save, and resume it; resume sends CInput the esStart message, whose handler reads once
 * while the game is still stopped, then recentres twice and clears the stoppage. The loop's GetCursorPos then returns
 * the cursor, and the policy timer follows. */
static void quick_save_in_ui_loop(void) {
    H.stopped = 1;
    H.cursor_x = CLIENT_W / 2;
    H.cursor_y = CLIENT_H / 2;
    lifted_read();
    H.cursor_x = CLIENT_W / 2;
    H.cursor_y = CLIENT_H / 2;
    H.stopped = 0;
    if (H.update_after_hooks) host_update();
    host_read_cursor();
    host_update();
}

static frame_t quick_save_between(rig_t* r, int recapture, int update_after_hooks, const step_t* before, const step_t* after) {
    H.host_flips = 1;
    H.recapture = recapture;
    H.update_after_hooks = update_after_hooks;
    H.real_x = CLIENT_W / 2 + 100;
    H.real_y = 10;
    if (before) run_step(r, before);
    quick_save_in_ui_loop();
    return run_step(r, after);
}

static int no_rotation(const frame_t* f) { return f->rotate_x == 0 && f->rotate_y == 0; }

static void test_quick_save_recapture(void) {
    static const step_t idle = { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 };
    static const step_t moving = { 0, 30, -40, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 };
    static const step_t drift = { 0, 10, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 };
    static const step_t stick_before = { 0, 20, 0, 1.0f, 0.0f, 0.0f, DT_X20, 0 };
    static const step_t stick_held = { 0, 20, 0, 1.0f, 0.0f, 0.0f, DT_X20, 0 };
    rig_t r;
    frame_t f;
    for (int pads = 0; pads < 2; pads++) {
        /* the host as it was: the real cursor, read in the stale state after the resume, is the next frame's delta */
        rig_init(&r, pads, 0);
        f = quick_save_between(&r, 0, 0, NULL, &idle);
        CHECK(f.read_x == CLIENT_W / 2 + 100 && f.read_y == 10);
        CHECK(fabsf(rot(f.rotate_x) - 0.5f) < 1e-5f && fabsf(rot(f.rotate_y) - 1.87f) < 1e-5f); /* looks up */
        /* either guard alone, and both: no delta, with the mouse still, moving before, and the stick neutral or used */
        for (int guards = 1; guards < 4; guards++) {
            int recapture = guards & 1, after_hooks = (guards & 2) != 0;
            rig_init(&r, pads, 0);
            f = quick_save_between(&r, recapture, after_hooks, NULL, &idle);
            CHECK(no_rotation(&f) && f.read_x == CLIENT_W / 2 && f.read_y == CLIENT_H / 2);
            CHECK(f.after_x == CLIENT_W / 2 && f.after_y == CLIENT_H / 2);
            rig_init(&r, pads, 0);
            f = quick_save_between(&r, recapture, after_hooks, pads ? &stick_before : &moving, &idle);
            CHECK(no_rotation(&f));
            rig_init(&r, pads, 0);
            f = quick_save_between(&r, recapture, after_hooks, NULL, &drift);
            CHECK(f.read_x == CLIENT_W / 2 + (pads ? 0 : 10)); /* motion after the save is not lost (the stick carries its own) */
            if (!pads) CHECK(fabsf(rot(f.rotate_x) - 0.05f) < 1e-6f && f.rotate_y == 0);
            if (pads) {
                rig_init(&r, pads, 0);
                f = quick_save_between(&r, recapture, after_hooks, &stick_before, &stick_held);
                CHECK(fabsf(rot(f.rotate_x) - 0.1f) < 1e-6f && f.rotate_y == 0); /* the held stick goes on, once */
            }
        }
    }
    /* repeated saves leave no trace: every frame in between is still */
    rig_init(&r, 0, 0);
    for (int i = 0; i < 5; i++) {
        f = quick_save_between(&r, 1, 1, i ? NULL : &moving, &idle);
        CHECK(no_rotation(&f));
    }
    /* the recapture is only for a cursor that was the real one: a released pointer keeps its own */
    {
        long x = 7, y = 9;
        CHECK(!pointer_recapture(INPUT_GAMEPLAY_RELEASED, INPUT_GAMEPLAY_CAPTURED, &x, &y, CLIENT_W, CLIENT_H) && x == 7 && y == 9);
        CHECK(!pointer_recapture(INPUT_DIALOG, INPUT_DIALOG, &x, &y, CLIENT_W, CLIENT_H) && x == 7);
        CHECK(!pointer_recapture(INPUT_DIALOG, INPUT_GAMEPLAY_CAPTURED, &x, &y, 0, 0) && x == 7);
        CHECK(pointer_recapture(INPUT_DIALOG, INPUT_GAMEPLAY_CAPTURED, &x, &y, CLIENT_W, CLIENT_H) && x == CLIENT_W / 2 &&
              y == CLIENT_H / 2);
        x = 1;
        CHECK(pointer_recapture(INPUT_MENU, INPUT_GAMEPLAY_CAPTURED, &x, &y, 641, 481) && x == 320 && y == 240);
    }
}

/* Mouse sensitivity reaches CPlayer as CE's MouseSensX/MouseSensY do: the game's rotation is sensitivity x counts x
 * 0.005 on both axes, Invert mouse still turns only the vertical, and the gamepad's look is not touched. */
static void test_mouse_sensitivity(void) {
    static const float k[] = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f };
    static const step_t look = { 0, 20, -12, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 };
    static const step_t idle = { 0, 0, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 };
    static const step_t stick = { 0, 0, 0, 1.0f, -1.0f, 0.0f, DT_X20, 0 };
    rig_t r;
    frame_t f, plain;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        for (int invert = 0; invert < 2; invert++) {
            rig_init(&r, 0, invert);
            H.sensitivity = k[i];
            f = run_step(&r, &look);
            CHECK(fabsf(rot(f.rotate_x) - (float)(long)(20 * k[i]) * 0.005f) < 1e-6f);
            CHECK(fabsf(rot(f.rotate_y) - (invert ? -1.0f : 1.0f) * (float)(long)(12 * k[i]) * 0.005f) < 1e-6f);
            CHECK(f.after_x == CLIENT_W / 2 && f.after_y == CLIENT_H / 2); /* CInput recentres as always */
            f = run_step(&r, &idle);
            CHECK(rot(f.rotate_x) == 0.0f && rot(f.rotate_y) == 0.0f); /* nothing left over turns the view once the mouse stops */
        }
    /* 1.00 is the game exactly */
    rig_init(&r, 0, 0);
    plain = run_step(&r, &look);
    rig_init(&r, 0, 0);
    H.sensitivity = 1.0f;
    f = run_step(&r, &look);
    CHECK(field_differs(&plain, &f) == NULL);
    /* the stick is the same at any mouse sensitivity */
    rig_init(&r, 1, 0);
    run_step(&r, &stick);
    plain = run_step(&r, &stick);
    rig_init(&r, 1, 0);
    H.sensitivity = 3.0f;
    run_step(&r, &stick);
    f = run_step(&r, &stick);
    CHECK(field_differs(&plain, &f) == NULL && !no_rotation(&f));
    /* slow motion below one count at 0.25 still turns, a count every fourth frame */
    {
        const step_t crawl = { 0, 1, 0, 0.0f, 0.0f, 0.0f, DT_FRAME, 0 };
        float turned = 0.0f;
        rig_init(&r, 0, 0);
        H.sensitivity = 0.25f;
        for (int i = 0; i < 8; i++) turned += rot(run_step(&r, &crawl).rotate_x);
        CHECK(fabsf(turned - 2 * 0.005f) < 1e-6f);
    }
    /* F7 between two frames at a high sensitivity: still no jump */
    rig_init(&r, 0, 0);
    H.sensitivity = 4.0f;
    f = quick_save_between(&r, 1, 1, &look, &idle);
    CHECK(no_rotation(&f) && f.read_x == CLIENT_W / 2 && f.read_y == CLIENT_H / 2);
}

int main(void) {
    test_profile_activate();
    uintptr_t lo = (uintptr_t)g_window, hi = lo + sizeof g_window;
    int from_exe;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (lo > WINDOW_LO || hi < WINDOW_HI) {
        printf("guest window 0x%08X-0x%08X does not cover 0x%08X-0x%08X: link at /BASE:0x00400000 /FIXED\n",
               (unsigned)lo, (unsigned)hi, WINDOW_LO, WINDOW_HI);
        return 2;
    }
    from_exe = load_game_data();
    printf("key map and CInput constants %s\n", from_exe ? "read from trespass.exe" : "built in (trespass.exe not found)");
    test_sequences();
    test_pauses();
    test_quick_save_recapture();
    test_mouse_sensitivity();
    printf("input tests: %lu checks, %lu failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
