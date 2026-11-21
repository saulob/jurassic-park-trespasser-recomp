#include "guest_ui_model.h"

#include <string.h>

#include "guest_va.h"

#define MANAGER_OFFSET 0x08u
#define STACK_BEGIN_OFFSET 0x38u
#define STACK_END_OFFSET 0x3Cu
#define GAME_WND_STOPPED_OFFSET 0x30u
#define MAX_STACK 32u

static const struct {
    unsigned vtable_id;
    const char* name;
    guest_wnd_t kind;
} k_classes[] = {
    { GVA_ID(0x00645C3C), "CGameWnd", GUEST_WND_GAME },
    { GVA_ID(0x00646B40), "CInGameOptionsWnd", GUEST_WND_INGAME_OPTIONS },
    { GVA_ID(0x00646D10), "CHintWnd", GUEST_WND_OTHER },
    { GVA_ID(0x00645F38), "CControlsWnd", GUEST_WND_OTHER },
    { GVA_ID(0x00646970), "CQuitWnd", GUEST_WND_QUIT },
    { GVA_ID(0x00645E24), "CYesNoMsgDlg", GUEST_WND_OTHER },
    { GVA_ID(0x00645D34), "CYesNo", GUEST_WND_OTHER },
    { GVA_ID(0x006461B8), "CYesNoTextDlg", GUEST_WND_OTHER },
    { GVA_ID(0x00646390), "COKDlg", GUEST_WND_OTHER },
    { GVA_ID(0x00646480), "CMsgOkDlg", GUEST_WND_OTHER },
    { GVA_ID(0x006466B8), "CMainScreenWnd", GUEST_WND_MAIN_MENU },
    { GVA_ID(0x006467A0), "CLoaderWnd", GUEST_WND_LOADER },
    { GVA_ID(0x00646FC8), "CVideoWnd", GUEST_WND_VIDEO },
    { GVA_ID(0x00646A58), "COptionsWnd", GUEST_WND_OTHER },
    { GVA_ID(0x006462A8), "CSaveGameWnd", GUEST_WND_OTHER },
    { GVA_ID(0x006460D0), "CLoadGameWnd", GUEST_WND_OTHER },
    { GVA_ID(0x00646C28), "CAudioWnd", GUEST_WND_OTHER },
    { GVA_ID(0x00645B4C), "CRenderWnd", GUEST_WND_OTHER },
    { GVA_ID(0x00646888), "CDirectLoadWnd", GUEST_WND_OTHER },
};

const char* guest_ui_model_class_name(const uint32_t* gva, uint32_t vtable) {
    for (size_t i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++)
        if (gva[k_classes[i].vtable_id] == vtable) return k_classes[i].name;
    return "a window of the game";
}

guest_wnd_t guest_ui_model_kind(const uint32_t* gva, uint32_t vtable) {
    for (size_t i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++)
        if (gva[k_classes[i].vtable_id] == vtable) return k_classes[i].kind;
    return GUEST_WND_OTHER;
}

int guest_ui_model_probe(const uint32_t* gva, guest_read32_fn read32, void* context, guest_ui_view_t* out) {
    uint32_t main_wnd, manager, begin, end, top, stopped;
    uint32_t game_vtable = gva[GVA_ID(0x00645C3C)];
    memset(out, 0, sizeof *out);
    out->ui = GUEST_UI_NONE;
    out->kind = GUEST_WND_NONE;
    if (!read32(context, gva[GVA_ID(0x0066E010)], &main_wnd) || !main_wnd || !read32(context, main_wnd + MANAGER_OFFSET, &manager) || !manager) return 0;
    out->manager = manager;
    if (!read32(context, manager + STACK_BEGIN_OFFSET, &begin) || !read32(context, manager + STACK_END_OFFSET, &end) || end <= begin ||
        (end - begin) % 4 || (end - begin) / 4 > MAX_STACK)
        return 0;
    out->depth = (end - begin) / 4;
    if (!read32(context, end - 4, &top) || !top || !read32(context, top, &out->top_vtable)) return 0;
    out->top = top;
    out->kind = guest_ui_model_kind(gva, out->top_vtable);
    if (out->top_vtable == game_vtable) {
        if (!read32(context, top + GAME_WND_STOPPED_OFFSET, &stopped)) return 0;
        out->stopped = stopped != 0;
        out->ui = out->stopped ? GUEST_UI_DIALOG : GUEST_UI_GAMEPLAY;
        return 1;
    }
    out->ui = GUEST_UI_MENU;
    for (uint32_t i = 0; i + 1 < out->depth; i++) {
        uint32_t w, v;
        if (read32(context, begin + 4 * i, &w) && w && read32(context, w, &v) && v == game_vtable) out->ui = GUEST_UI_DIALOG;
    }
    return 1;
}
