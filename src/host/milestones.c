/* Logs the first call to a few imports that mark boot stages. Observation only: the call proceeds unchanged. */
#include "host.h"

#include <string.h>

typedef struct {
    const char* import;
    const char* stage;
    uint32_t va;
    int reached;
} milestone_t;

static milestone_t s_milestones[] = {
    { "RegisterClassA", "window class registration" },
    { "CreateWindowExA", "window creation" },
    { "DirectDrawEnumerateA", "DirectDraw device enumeration" },
    { "DirectDrawCreate", "DirectDraw object creation" },
    { "_SmackOpen@12", "Smacker video open" },
    { "PeekMessageA", "message loop" },
};
#define MILESTONE_COUNT ((int)(sizeof s_milestones / sizeof s_milestones[0]))

static int s_pending;

static uint32_t bound_import(const char* wanted) {
    uint8_t* base = (uint8_t*)(uintptr_t)JPT_GUEST_IMAGE_BASE;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; dir.VirtualAddress && d->Name; d++) {
        uint32_t* names = (uint32_t*)(base + d->OriginalFirstThunk);
        uint32_t* slots = (uint32_t*)(base + d->FirstThunk);
        if (!d->OriginalFirstThunk) continue;
        for (; *names; names++, slots++)
            if (!(*names & 0x80000000u) && !strcmp((const char*)(base + *names + 2), wanted)) return *slots;
    }
    return 0;
}

void milestones_init(void) {
    s_pending = 0;
    for (int i = 0; i < MILESTONE_COUNT; i++) {
        s_milestones[i].va = bound_import(s_milestones[i].import);
        if (s_milestones[i].va) s_pending++;
        else host_log("stage", "import %s not found; its stage will not be reported", s_milestones[i].import);
    }
}

void milestones_observe(uint32_t target) {
    if (!s_pending) return;
    for (int i = 0; i < MILESTONE_COUNT; i++) {
        milestone_t* m = &s_milestones[i];
        if (m->va != target || m->reached) continue;
        m->reached = 1;
        s_pending--;
        host_log("stage", "%s: first call to %s, from sub_%08X", m->stage, m->import, g_cur_func);
    }
}
