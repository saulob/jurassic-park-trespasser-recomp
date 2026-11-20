/* Argument counts of imports the guest calls that have no implementation yet. Used only by the permissive diagnostic mode
 * (hle_set_permissive), which lets a headless run read past a missing import to list what the guest calls next. */
#include <string.h>

#include "linux_hle.h"

typedef struct {
    const char* dll;
    const char* name;
    int argc;
    int conv;
} signature_t;

#define S(dll, name, argc) { dll, #name, argc, HT_STDCALL }

static const signature_t k_signatures[] = {
    S("user32.dll", RegisterClassA, 1), S("user32.dll", GetCursorPos, 1), S("user32.dll", TranslateAcceleratorA, 3),
    S("user32.dll", TranslateMessage, 1), S("user32.dll", KillTimer, 2), S("user32.dll", RegisterWindowMessageA, 1),
    S("user32.dll", DispatchMessageA, 1), S("user32.dll", UnionRect, 3), S("user32.dll", ShowCursor, 1),
    S("user32.dll", SetTimer, 4), S("user32.dll", PeekMessageA, 5), S("user32.dll", InvalidateRect, 3),
    S("user32.dll", ValidateRect, 2), S("user32.dll", IsIconic, 1), S("user32.dll", SetCapture, 1),
    S("user32.dll", GetForegroundWindow, 0), S("user32.dll", GetWindowLongA, 2), S("user32.dll", ReleaseCapture, 0),
    S("user32.dll", LoadCursorA, 2), S("user32.dll", SetCursor, 1), S("user32.dll", SetWindowTextA, 2),
    S("user32.dll", wvsprintfA, 3), S("user32.dll", CreateWindowExA, 12), S("user32.dll", ShowWindow, 2),
    S("user32.dll", UpdateWindow, 1), S("user32.dll", GetSystemMenu, 2), S("user32.dll", EnableMenuItem, 3),
    S("user32.dll", PostQuitMessage, 1), S("user32.dll", DestroyWindow, 1), S("user32.dll", EnumWindows, 2),
    S("user32.dll", GetAsyncKeyState, 1), S("user32.dll", GetClientRect, 2), S("user32.dll", ClientToScreen, 2),
    S("user32.dll", SetCursorPos, 2), S("user32.dll", IsDlgButtonChecked, 2), S("user32.dll", SetDlgItemTextA, 3),
    S("user32.dll", EnableWindow, 2), S("user32.dll", CheckDlgButton, 3), S("user32.dll", SendMessageA, 4),
    S("user32.dll", MessageBoxA, 4), S("user32.dll", GetDlgItem, 2), S("user32.dll", RemovePropA, 2),
    S("user32.dll", GetPropA, 2), S("user32.dll", SetPropA, 3), S("user32.dll", EndDialog, 2),
    S("user32.dll", CreateDialogIndirectParamA, 5), S("user32.dll", CreateDialogParamA, 5), S("user32.dll", DialogBoxParamA, 5),
    S("user32.dll", SetWindowLongA, 3), S("user32.dll", DefWindowProcA, 4), S("user32.dll", GetWindowRect, 2),
    S("user32.dll", GetSystemMetrics, 1), S("user32.dll", SetWindowPos, 7), S("user32.dll", LoadStringA, 4),
    S("user32.dll", BeginPaint, 2), S("user32.dll", GetWindowPlacement, 2), S("user32.dll", EndPaint, 2),
    S("user32.dll", GetParent, 1), S("user32.dll", GetActiveWindow, 0), S("user32.dll", MoveWindow, 6),
    S("user32.dll", SetActiveWindow, 1), S("user32.dll", InflateRect, 3), S("user32.dll", FillRect, 3), S("user32.dll", OffsetRect, 3),
    S("user32.dll", IntersectRect, 3), S("user32.dll", PtInRect, 3), S("user32.dll", SetRect, 5),
    { "user32.dll", "wsprintfA", 16, HT_CDECL }, S("user32.dll", GetDC, 1), S("user32.dll", ReleaseDC, 2),
    S("user32.dll", LoadIconA, 2), S("user32.dll", GetKeyState, 1),
    S("gdi32.dll", SelectClipRgn, 2), S("gdi32.dll", CreateFontIndirectA, 1), S("gdi32.dll", GetTextMetricsA, 2),
    S("gdi32.dll", SetSystemPaletteUse, 2), S("gdi32.dll", CreateCompatibleDC, 1), S("gdi32.dll", GetStockObject, 1),
    S("gdi32.dll", DeleteObject, 1), S("gdi32.dll", CreateFontA, 14), S("gdi32.dll", CreateSolidBrush, 1),
    S("gdi32.dll", SetBkMode, 2), S("gdi32.dll", SetTextColor, 2), S("gdi32.dll", SelectObject, 2),
    S("gdi32.dll", SetMapMode, 2), S("gdi32.dll", GetDeviceCaps, 2), S("gdi32.dll", BitBlt, 9),
    S("gdi32.dll", TextOutA, 5), S("gdi32.dll", CreateDIBSection, 6), S("gdi32.dll", StretchBlt, 11),
    S("gdi32.dll", SetStretchBltMode, 2), S("gdi32.dll", GetTextColor, 1), S("gdi32.dll", DeleteDC, 1),
    S("gdi32.dll", CreatePen, 3), S("gdi32.dll", Ellipse, 5), S("gdi32.dll", GetTextExtentPointA, 4),
    S("gdi32.dll", SetTextAlign, 2), S("gdi32.dll", CreateRectRgn, 4), S("gdi32.dll", OffsetWindowOrgEx, 3),
    S("gdi32.dll", LineTo, 3), S("gdi32.dll", SetBkColor, 2), S("gdi32.dll", MoveToEx, 4),
    S("ddraw.dll", DirectDrawCreate, 3), S("ddraw.dll", DirectDrawEnumerateA, 2),
    S("winmm.dll", joyGetPosEx, 2),
    S("kernel32.dll", CreateThread, 6), S("kernel32.dll", ResumeThread, 1), S("kernel32.dll", SuspendThread, 1),
    S("kernel32.dll", TerminateThread, 2), S("kernel32.dll", GetDiskFreeSpaceA, 5), S("kernel32.dll", WinExec, 2),
    S("kernel32.dll", FindResourceA, 3), S("kernel32.dll", LoadResource, 2), S("kernel32.dll", LockResource, 1),
    S("kernel32.dll", FreeResource, 1),
};

int hle_signature(const char* dll, const char* name, int* argc, int* conv) {
    for (size_t i = 0; i < sizeof k_signatures / sizeof k_signatures[0]; i++)
        if (!strcmp(k_signatures[i].dll, dll) && !strcmp(k_signatures[i].name, name)) {
            *argc = k_signatures[i].argc;
            *conv = k_signatures[i].conv;
            return 1;
        }
    return 0;
}
