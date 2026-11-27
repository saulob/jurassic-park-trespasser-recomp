#include "launch_handoff.h"

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "guest_abi.h"

#define CLASS_NAME "JptLaunchHandoff"
#define WM_HANDOFF_HIDE (WM_APP + 1)
#define WM_HANDOFF_FAILED (WM_APP + 2)
#define WM_HANDOFF_FRONT (WM_APP + 3)
#define SPIN_TIMER 1
#define SPIN_MS 33
#define SHOWN_WAIT_MS 3000
#define SPIN_DOTS 12

static struct {
    CRITICAL_SECTION lock;
    int active;
    jpt_handoff_t h;
    HANDLE thread, shown;
    HWND window, launcher, game;
    RECT rect;
    COLORREF bg, text, dim, accent;
    DWORD started;
    unsigned frame;
    RECT close_button;
    int phase_logged;
} H;

static COLORREF colour(unsigned rgb) { return RGB(rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF); }

static COLORREF mix(COLORREF a, COLORREF b, float t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

static DWORD elapsed(void) { return GetTickCount() - H.started; }

static float dpi_scale(HWND window) {
    typedef UINT(WINAPI * dpi_fn)(HWND);
    static dpi_fn get_dpi;
    static int looked;
    if (!looked) {
        get_dpi = (dpi_fn)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
        looked = 1;
    }
    return get_dpi ? (float)get_dpi(window) / 96.0f : 1.0f;
}

static void draw_text(HDC dc, const char* utf8, RECT* box, HFONT font, COLORREF c, UINT flags) {
    wchar_t wide[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0]))) wide[0] = 0;
    SelectObject(dc, font);
    SetTextColor(dc, c);
    DrawTextW(dc, wide, -1, box, flags | DT_NOPREFIX);
}

static HFONT make_font(float px, int weight) {
    return CreateFontA(-(int)px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, "Segoe UI");
}

static void draw_spinner(HDC dc, int cx, int cy, float scale) {
    const float radius = 18.0f * scale, dot = 3.2f * scale;
    HPEN pen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    for (int i = 0; i < SPIN_DOTS; i++) {
        const float angle = (float)i / SPIN_DOTS * 6.2831853f;
        const float lag = (float)((i - (int)(H.frame % SPIN_DOTS) + SPIN_DOTS) % SPIN_DOTS) / SPIN_DOTS;
        const int x = cx + (int)(sinf(angle) * radius), y = cy - (int)(cosf(angle) * radius);
        HBRUSH brush = CreateSolidBrush(mix(H.accent, H.bg, lag * 0.85f));
        HBRUSH old = (HBRUSH)SelectObject(dc, brush);
        Ellipse(dc, x - (int)dot, y - (int)dot, x + (int)dot + 1, y + (int)dot + 1);
        SelectObject(dc, old);
        DeleteObject(brush);
    }
    SelectObject(dc, pen);
}

static void paint(HWND window, HDC target) {
    RECT client, box;
    jpt_handoff_t h;
    char status[256];
    const char* key;
    float scale = dpi_scale(window);
    HDC dc;
    HBITMAP bitmap, old_bitmap;
    HFONT title_font, status_font;
    HBRUSH bg;
    int w, h_px, mid;
    EnterCriticalSection(&H.lock);
    h = H.h;
    LeaveCriticalSection(&H.lock);
    GetClientRect(window, &client);
    w = client.right;
    h_px = client.bottom;
    dc = CreateCompatibleDC(target);
    bitmap = CreateCompatibleBitmap(target, w > 0 ? w : 1, h_px > 0 ? h_px : 1);
    old_bitmap = (HBITMAP)SelectObject(dc, bitmap);
    bg = CreateSolidBrush(H.bg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    title_font = make_font(30.0f * scale, FW_SEMIBOLD);
    status_font = make_font(17.0f * scale, FW_NORMAL);
    mid = h_px / 2;
    key = jpt_handoff_status(&h);
    if (strstr(key, "%s")) snprintf(status, sizeof status, jpt_launcher_text(key), h.level);
    else snprintf(status, sizeof status, "%s", jpt_launcher_text(key));
    box = client;
    box.top = mid - (int)(70 * scale);
    box.bottom = mid - (int)(20 * scale);
    draw_text(dc, jpt_launcher_text(jpt_handoff_title(&h)), &box, title_font, h.state == JPT_HANDOFF_FAILED ? H.accent : H.text,
              DT_CENTER | DT_SINGLELINE | DT_BOTTOM);
    if (h.state == JPT_HANDOFF_FAILED) {
        char reason[JPT_HANDOFF_TEXT_MAX + 32];
        HBRUSH button = CreateSolidBrush(H.accent);
        snprintf(reason, sizeof reason, "%s (%s)", h.reason, h.stage);
        box = client;
        box.left += (int)(40 * scale);
        box.right -= (int)(40 * scale);
        box.top = mid - (int)(10 * scale);
        box.bottom = mid + (int)(20 * scale);
        draw_text(dc, reason, &box, status_font, H.text, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        box.top = box.bottom;
        box.bottom = box.top + (int)(30 * scale);
        draw_text(dc, status, &box, status_font, H.dim, DT_CENTER | DT_SINGLELINE);
        H.close_button.left = w / 2 - (int)(70 * scale);
        H.close_button.right = w / 2 + (int)(70 * scale);
        H.close_button.top = mid + (int)(70 * scale);
        H.close_button.bottom = mid + (int)(108 * scale);
        FillRect(dc, &H.close_button, button);
        DeleteObject(button);
        box = H.close_button;
        draw_text(dc, jpt_launcher_text("Close"), &box, status_font, H.bg, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else {
        box = client;
        box.top = mid - (int)(10 * scale);
        box.bottom = mid + (int)(20 * scale);
        draw_text(dc, status, &box, status_font, H.dim, DT_CENTER | DT_SINGLELINE);
        draw_spinner(dc, w / 2, mid + (int)(70 * scale), scale);
    }
    BitBlt(target, 0, 0, w, h_px, dc, 0, 0, SRCCOPY);
    DeleteObject(title_font);
    DeleteObject(status_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static void dismiss(HWND window) {
    EnterCriticalSection(&H.lock);
    jpt_handoff_dismissed(&H.h);
    LeaveCriticalSection(&H.lock);
    DestroyWindow(window);
}

static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        paint(window, dc);
        EndPaint(window, &ps);
        if (H.shown) SetEvent(H.shown);
        return 0;
    }
    case WM_TIMER:
        H.frame++;
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_HANDOFF_FRONT:
        SetForegroundWindow(window);
        return 0;
    case WM_HANDOFF_HIDE:
        if ((HWND)wp) SetForegroundWindow((HWND)wp);
        DestroyWindow(window);
        return 0;
    case WM_HANDOFF_FAILED:
        KillTimer(window, SPIN_TIMER);
        SetForegroundWindow(window);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (H.h.state == JPT_HANDOFF_FAILED && PtInRect(&H.close_button, p)) dismiss(window);
        return 0;
    }
    case WM_KEYDOWN:
        if (H.h.state == JPT_HANDOFF_FAILED && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_SPACE)) dismiss(window);
        return 0;
    case WM_CLOSE:
        if (H.h.state == JPT_HANDOFF_FAILED) dismiss(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, msg, wp, lp);
    }
}

static DWORD WINAPI handoff_thread(LPVOID unused) {
    WNDCLASSW wc;
    MSG msg;
    HWND window;
    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_APPSTARTING);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"" CLASS_NAME;
    RegisterClassW(&wc);
    window = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Jurassic Park: Trespasser", WS_POPUP, H.rect.left, H.rect.top,
                             H.rect.right - H.rect.left, H.rect.bottom - H.rect.top, NULL, NULL, wc.hInstance, NULL);
    if (!window) {
        SetEvent(H.shown);
        return 1;
    }
    EnterCriticalSection(&H.lock);
    H.window = window;
    LeaveCriticalSection(&H.lock);
    SetTimer(window, SPIN_TIMER, SPIN_MS, NULL);
    SetWindowPos(window, H.launcher ? H.launcher : HWND_TOP, H.rect.left, H.rect.top, H.rect.right - H.rect.left, H.rect.bottom - H.rect.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(window);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnterCriticalSection(&H.lock);
    H.window = NULL;
    LeaveCriticalSection(&H.lock);
    return 0;
}

void launch_handoff_begin(const jpt_launch_handoff_request_t* request) {
    int shown;
    if (H.active) return;
    InitializeCriticalSection(&H.lock);
    H.active = 1;
    H.started = GetTickCount();
    jpt_handoff_init(&H.h, 1);
    jpt_handoff_request(&H.h, request->custom, request->level);
    H.launcher = (HWND)request->window;
    if (!H.launcher || !GetWindowRect(H.launcher, &H.rect) || IsIconic(H.launcher)) {
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        SetRect(&H.rect, (sw - 1280) / 2, (sh - 720) / 2, (sw + 1280) / 2, (sh + 720) / 2);
    }
    H.bg = colour(request->background);
    H.text = colour(request->text);
    H.dim = colour(request->dim);
    H.accent = colour(request->accent);
    host_log("launch", "requested type=%s%s%s presenter=handoff_window", request->custom ? "custom" : "campaign",
             request->custom && request->level && *request->level ? " package=" : "", request->custom && request->level ? request->level : "");
    H.shown = CreateEventW(NULL, TRUE, FALSE, NULL);
    H.thread = CreateThread(NULL, 0, handoff_thread, NULL, 0, NULL);
    shown = H.thread && H.shown && WaitForSingleObject(H.shown, SHOWN_WAIT_MS) == WAIT_OBJECT_0 && H.window;
    EnterCriticalSection(&H.lock);
    if (shown) jpt_handoff_shown(&H.h);
    else H.h.presenter = 0;
    LeaveCriticalSection(&H.lock);
    if (shown) host_log("launch", "handoff window shown below the launcher (%lu ms)", (unsigned long)elapsed());
    else host_log("launch", "the handoff window could not be shown: the launcher closes without one");
}

void launch_handoff_launcher_closed(void) {
    HWND window;
    if (!H.active) return;
    EnterCriticalSection(&H.lock);
    jpt_handoff_launcher_closed(&H.h);
    jpt_handoff_phase(&H.h, JPT_HANDOFF_PHASE_RUNTIME);
    window = H.window;
    LeaveCriticalSection(&H.lock);
    if (window) PostMessageW(window, WM_HANDOFF_FRONT, 0, 0);
    host_log("launch", "launcher closed (%lu ms), %s", (unsigned long)elapsed(), window ? "the handoff window shows" : "no window shows until the game's");
}

void launch_handoff_phase(jpt_handoff_phase_t phase) {
    HWND window;
    if (!H.active) return;
    EnterCriticalSection(&H.lock);
    jpt_handoff_phase(&H.h, phase);
    window = H.window;
    LeaveCriticalSection(&H.lock);
    if (window) InvalidateRect(window, NULL, FALSE);
    host_log("launch", "phase=%s (%lu ms)", jpt_handoff_phase_stage(phase), (unsigned long)elapsed());
}

void launch_handoff_game_window_created(void) {
    if (!H.active) return;
    EnterCriticalSection(&H.lock);
    jpt_handoff_game_window_created(&H.h);
    LeaveCriticalSection(&H.lock);
    host_log("launch", "game window created (%lu ms)", (unsigned long)elapsed());
}

void launch_handoff_game_window_ready(void* game_window) {
    HWND window;
    int hide;
    if (!H.active || H.h.state == JPT_HANDOFF_COMPLETE) return;
    EnterCriticalSection(&H.lock);
    jpt_handoff_game_window_ready(&H.h);
    hide = jpt_handoff_should_hide(&H.h);
    window = H.window;
    LeaveCriticalSection(&H.lock);
    /* Not waited for: the handoff thread's activation of the game window needs this thread to pump. */
    if (hide && window) PostMessageW(window, WM_HANDOFF_HIDE, (WPARAM)game_window, 0);
    EnterCriticalSection(&H.lock);
    jpt_handoff_hidden(&H.h);
    LeaveCriticalSection(&H.lock);
    host_log("launch", "game window ready (%lu ms): handoff %s, %u moment(s) without one of our windows", (unsigned long)elapsed(),
             jpt_handoff_state_name(H.h.state), H.h.blank_moments);
}

void launch_handoff_finish(int exit_code) {
    HWND window;
    char reason[96];
    if (!H.active) return;
    EnterCriticalSection(&H.lock);
    window = H.window;
    if (H.h.state != JPT_HANDOFF_COMPLETE) {
        snprintf(reason, sizeof reason, "the game stopped with exit code %d before its window was ready", exit_code);
        jpt_handoff_fail(&H.h, reason);
    }
    LeaveCriticalSection(&H.lock);
    if (H.h.state == JPT_HANDOFF_FAILED) {
        host_log("launch", "failed stage=%s reason=%s", H.h.stage, H.h.reason);
        if (window) {
            PostMessageW(window, WM_HANDOFF_FAILED, 0, 0);
            WaitForSingleObject(H.thread, INFINITE);
        }
    }
    if (H.thread) CloseHandle(H.thread);
    if (H.shown) CloseHandle(H.shown);
    H.thread = H.shown = NULL;
    H.active = 0;
}
