#!/usr/bin/env python3
"""Win32 import inventory of the guest images, classified by the Linux port's blocker categories.

Reads the PE import tables of the game executable and its DLLs (no third-party modules) and, when the lifted C is
unpacked, counts how many lifted references (calls or loads of the IAT slot) reach each import. Prints a table, or JSON with --json.
  usage: tools/linux_inventory.py [--game DIR] [--gen DIR] [--json]
"""
import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

CATEGORY = {
    "A": "Win32 ABI (guest calling conventions, structures)",
    "B": "Win32 host implementation (kernel objects, handles)",
    "C": "filesystem/path",
    "D": "registry/config",
    "E": "window/input",
    "F": "DirectDraw/software renderer",
    "G": "audio",
    "H": "video/Smacker",
    "I": "CRT/runtime",
    "J": "threading/synchronization",
    "K": "timers",
    "L": "dynamic modules",
}

KERNEL_RULES = [
    (r"^(CreateFile|ReadFile|WriteFile|SetFilePointer|SetEndOfFile|FlushFileBuffers|GetFileSize|GetFileType|GetFileAttributes|"
     r"SetFileAttributes|DeleteFile|MoveFile|CopyFile|FindFirstFile|FindNextFile|FindClose|GetCurrentDirectory|"
     r"SetCurrentDirectory|CreateDirectory|RemoveDirectory|GetFullPathName|GetTempPath|GetTempFileName|GetDriveType|"
     r"GetLogicalDrives|GetDiskFreeSpace|GetVolumeInformation|GetFileTime|SetFileTime|FileTimeTo|GetStdHandle|"
     r"SetHandleCount|GetShortPathName|CloseHandle|DuplicateHandle|GetSystemDirectory|GetWindowsDirectory|GetModuleFileName|"
     r"SetStdHandle|SetErrorMode|GetFileInformationByHandle|LockFile|UnlockFile|OpenFile|_lopen|_lread|_lclose|_llseek|_lcreat|_lwrite)", "C"),
    (r"^(Create(Thread|Event|Mutex|Semaphore)|ExitThread|TerminateThread|SuspendThread|ResumeThread|WaitFor|SetEvent|ResetEvent|"
     r"PulseEvent|ReleaseMutex|ReleaseSemaphore|OpenEvent|OpenMutex|Sleep|SwitchToThread|GetCurrentThread|GetCurrentProcess|"
     r"GetThreadPriority|SetThreadPriority|Tls|InitializeCriticalSection|DeleteCriticalSection|EnterCriticalSection|"
     r"LeaveCriticalSection|TryEnterCriticalSection|Interlocked|GetExitCodeThread|SetPriorityClass|GetPriorityClass|"
     r"SetThreadAffinityMask|SetProcessAffinityMask|GetProcessAffinityMask|DisableThreadLibraryCalls|ExitProcess|TerminateProcess|"
     r"GetCurrentProcessId|GetCurrentThreadId|SetUnhandledExceptionFilter|UnhandledExceptionFilter|RaiseException|"
     r"GetExitCodeProcess|CreateProcess|WinExec)", "J"),
    (r"^(QueryPerformance|GetTickCount|timeGetTime|GetSystemTime|GetLocalTime|GetTimeZone|SystemTimeTo|FileTimeTo|"
     r"GetSystemTimeAsFileTime|timeBeginPeriod|timeEndPeriod|timeSetEvent|timeKillEvent|timeGetDevCaps|SetTimer|KillTimer)", "K"),
    (r"^(LoadLibrary|FreeLibrary|GetProcAddress|GetModuleHandle|LoadResource|FindResource|LockResource|SizeofResource|"
     r"LoadString|LoadImage|LoadIcon|LoadCursor|LoadBitmap|FreeResource|EnumResource)", "L"),
    (r"^(Reg|RegOpenKey|RegCreateKey|RegQuery|RegSet|RegDelete|RegClose|GetPrivateProfile|WritePrivateProfile|GetProfile|WriteProfile)", "D"),
    (r"^(waveOut|waveIn|mixer|midi|mci|PlaySound|sndPlay|DirectSound|auxGet|auxSet)", "G"),
    (r"^(joyGet|joySet|GetAsyncKeyState|GetKeyState|GetKeyboardState|GetCursorPos|SetCursorPos|ShowCursor|SetCursor|ClipCursor|"
     r"GetCapture|SetCapture|ReleaseCapture|GetFocus|SetFocus|GetActiveWindow|SetActiveWindow|GetForegroundWindow|"
     r"SetForegroundWindow|MapVirtualKey|VkKeyScan|GetMessage|PeekMessage|TranslateMessage|DispatchMessage|PostMessage|"
     r"SendMessage|PostQuitMessage|DefWindowProc|RegisterClass|CreateWindow|DestroyWindow|ShowWindow|UpdateWindow|MoveWindow|"
     r"SetWindow|GetWindow|GetClient|GetSystemMetrics|AdjustWindowRect|ClientToScreen|ScreenToClient|IsIconic|IsZoomed|"
     r"IsWindow|EnableWindow|InvalidateRect|ValidateRect|BeginPaint|EndPaint|GetDC|ReleaseDC|CreateDialog|DialogBox|EndDialog|"
     r"GetDlg|SetDlg|MessageBox|SetMenu|GetMenu|CreateMenu|AppendMenu|DestroyMenu|TrackPopupMenu|GetSystemMenu|DrawMenuBar|"
     r"EnableMenuItem|CheckMenuItem|CheckRadioButton|CheckDlgButton|IsDlgButton|GetParent|SetParent|FindWindow|BringWindowToTop|"
     r"SetRect|OffsetRect|InflateRect|PtInRect|IntersectRect|UnionRect|CopyRect|EqualRect|IsRectEmpty|SubtractRect|GetKeyboardType|"
     r"GetDesktopWindow|MonitorFrom|EnumDisplay|ChangeDisplaySettings|SystemParametersInfo|ShowCursor|LoadAccelerators|"
     r"TranslateAccelerator|SetWindowText|GetWindowText|wsprintf|wvsprintf|CharUpper|CharLower|CharNext|CharPrev|IsCharAlpha|"
     r"GetKeyNameText|GetWindowRect|GetWindowLong|SetWindowLong|GetClassLong|SetClassLong|SetWindowPos|GetWindowPlacement|"
     r"SetWindowPlacement|GetLastActivePopup|GetClassName|GetClassInfo|UnregisterClass|IsWindowVisible|GetSysColor|"
     r"GetSysColorBrush|CallWindowProc|CreateCaret|DestroyCaret|SetCaretPos|ShowCaret|HideCaret|MapWindowPoints|GetUpdateRect|"
     r"RedrawWindow|ScrollWindow|SetScrollInfo|GetScrollInfo|SetScrollRange|GetScrollRange|SetScrollPos|GetScrollPos|"
     r"DrawText|DrawIcon|DrawFocusRect|DrawEdge|DrawFrameControl|FillRect|FrameRect|InvertRect|GetMessagePos|GetMessageTime|"
     r"WaitMessage|InSendMessage|ReplyMessage|MsgWaitForMultipleObjects|GetInputState|GetQueueStatus|FlashWindow|"
     r"SetSysColors|ExitWindows|CreateIcon|DestroyIcon|DestroyCursor|CopyIcon|GetIconInfo|ScreenToClient|WindowFrom|ChildWindowFrom)", "E"),
    (r"^(CreateDIBSection|CreateCompatibleDC|CreateCompatibleBitmap|BitBlt|StretchBlt|StretchDIBits|SetDIBits|GetDIBits|"
     r"SelectObject|DeleteObject|DeleteDC|CreateBitmap|CreateSolidBrush|CreatePen|CreateFont|CreatePalette|SelectPalette|"
     r"RealizePalette|GetSystemPaletteEntries|SetPaletteEntries|GetPaletteEntries|AnimatePalette|GetDeviceCaps|SetTextColor|"
     r"SetBkColor|SetBkMode|TextOut|GetTextExtent|GetTextMetrics|GetStockObject|CreateDC|GdiFlush|GetObject|SetPixel|"
     r"GetPixel|MoveToEx|LineTo|Rectangle|Ellipse|Polygon|SetROP2|SetMapMode|SetStretchBltMode|CreateRectRgn|SelectClipRgn|"
     r"GetClipBox|IntersectClipRect|ExtTextOut|SetTextAlign|GetCharWidth|CreateFontIndirect|EnumFontFamilies|"
     r"CreateDIBitmap|CreateCompatible|PatBlt|MaskBlt|SetDIBColorTable|GetDIBColorTable|CreateHalftonePalette|CreateRoundRectRgn|"
     r"CombineRgn|PtInRegion|SetWindowRgn|GetRegionData|ExtCreateRegion|OffsetRgn|CreatePatternBrush|CreateHatchBrush|"
     r"CreateDIBPatternBrush|GetBitmapBits|SetBitmapBits|GetCurrentObject|SaveDC|RestoreDC|SetViewportOrg|SetWindowOrg)", "F"),
]

MSVCRT_RULES = [
    (r"^(_open|_read|_write|_close|_lseek|_tell|_eof|_chsize|_commit|_dup|_fileno|_get_osfhandle|_filelength|_sopen|_creat|_setmode|"
     r"_access|_chmod|_unlink|_stat|_fstat|_rmdir|_mkdir|_chdir|_getcwd|_findfirst|_findnext|_findclose|_fullpath|_splitpath|_makepath|"
     r"fopen|fclose|fread|fwrite|fseek|ftell|fgets|fputs|fgetc|fputc|fflush|feof|ferror|fscanf|fprintf|vfprintf|fgetpos|fsetpos|rewind|"
     r"setvbuf|setbuf|tmpfile|tmpnam|remove|rename|_unlink|_wfopen|_fsopen|_fdopen|_flushall|_fcloseall|_rmtmp|_tempnam|ungetc|getc|putc|"
     r"__p__iob|_iob|_pctype|_isctype|__p__pctype|__mb_cur_max|_amsg_exit|__getmainargs)", "C"),
    (r"^(_beginthread|_beginthreadex|_endthread|_endthreadex|_lock|_unlock|_cexit|_c_exit|exit|_exit|abort|_onexit|__dllonexit|atexit|"
     r"signal|raise|_XcptFilter|_except_handler3|_CxxThrowException|__CxxFrameHandler|_global_unwind2|_local_unwind2|__RTtypeid|"
     r"__RTDynamicCast|__RTCastToVoid|_setjmp|longjmp|_setjmp3|__set_app_type|__setusermatherr|_controlfp|_control87|_clearfp|_statusfp|"
     r"_fpreset|_set_new_handler|_set_new_mode|_initterm|_adjust_fdiv|_CIpow|_CIsin|_CIcos|_CItan|_CIatan|_CIatan2|_CIsqrt|_CIexp|"
     r"_CIlog|_CIlog10|_CIfmod|_ftol|_ftol2|_ltolower|_ltoupper|_purecall|_errno|__p__fmode|__p__commode|__p__environ|__p___argc|"
     r"__p___argv|__p__acmdln|_acmdln|__argc|__argv|_environ|_fmode|_commode|_winmajor|_winminor|_osver|_winver|_HUGE|_cexit)", "I"),
]


def classify(dll, name):
    dll = dll.lower()
    if dll.startswith("smackw32"):
        return "H"
    if dll.startswith(("ddraw", "d3d")):
        return "F"
    if dll.startswith("dsound"):
        return "G"
    if dll.startswith("msvcirt"):
        return "I"
    if dll.startswith("lz32"):
        return "C"
    if dll.startswith("comctl32"):
        return "E"
    if dll.startswith("advapi32"):
        return "D"
    if dll.startswith("processor"):
        return "L"
    rules = MSVCRT_RULES if dll.startswith("msvcrt") else KERNEL_RULES
    for pattern, cat in rules:
        if re.match(pattern, name):
            return cat
    if dll.startswith("msvcrt"):
        return "I"
    if dll.startswith("user32"):
        return "E"
    if dll.startswith("gdi32"):
        return "F"
    if dll.startswith("winmm"):
        return "K" if re.match(r"time", name) else "G"
    return "B"


def read_pe_imports(path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    if magic != 0x10B:
        raise ValueError(f"{path}: not PE32")
    image_base, = struct.unpack_from("<I", data, opt + 28)
    import_rva, import_size = struct.unpack_from("<II", data, opt + 104)
    secs = []
    sec = opt + opt_size
    for i in range(nsec):
        name = data[sec + i * 40:sec + i * 40 + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, sec + i * 40 + 8)
        secs.append((name, va, max(vsize, rsize), raw, rsize))

    def rva_off(rva):
        for _, va, size, raw, rsize in secs:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError(f"rva {rva:#x} outside the sections")

    def cstr(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("latin1")

    imports = []
    off = rva_off(import_rva) if import_rva else None
    while off is not None:
        orig, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, off)
        if not name_rva:
            break
        dll = cstr(rva_off(name_rva))
        thunk = rva_off(orig or first_thunk)
        slot = first_thunk
        while True:
            value, = struct.unpack_from("<I", data, thunk)
            if not value:
                break
            if value & 0x80000000:
                imports.append((dll, f"#{value & 0xFFFF}", image_base + slot))
            else:
                imports.append((dll, cstr(rva_off(value) + 2), image_base + slot))
            thunk += 4
            slot += 4
        off += 20
    return imports


def lifted_call_counts(gen_dir):
    counts = Counter()
    pattern = re.compile(r"MEM32\(0x([0-9A-Fa-f]{6,8})u?\)")
    for path in sorted(Path(gen_dir).glob("recomp_*.c")):
        text = path.read_text(errors="replace")
        for m in pattern.finditer(text):
            counts[int(m.group(1), 16)] += 1
    return counts


def main():
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parent.parent
    ap.add_argument("--game", default=str(root / "game"))
    ap.add_argument("--gen", default=str(root / "src" / "recomp" / "gen"))
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    game = Path(args.game)
    images = [p for p in (game / "trespass.exe", game / "SMACKW32.DLL", game / "MSVCIRT.DLL", game / "Processor.dll") if p.is_file()]
    if not images:
        print("linux inventory: DATA UNAVAILABLE (no game image under %s)" % game, file=sys.stderr)
        return 2
    calls = lifted_call_counts(args.gen) if Path(args.gen).is_dir() else Counter()

    rows = []
    for image in images:
        for dll, name, slot in read_pe_imports(image):
            rows.append({"image": image.name, "dll": dll.lower(), "name": name, "slot": slot,
                         "category": classify(dll, name), "lifted_sites": calls.get(slot, 0) if image.name == "trespass.exe" else None})

    if args.json:
        json.dump(rows, sys.stdout, indent=1)
        return 0

    by_image = defaultdict(list)
    for r in rows:
        by_image[r["image"]].append(r)
    for image, items in by_image.items():
        print(f"{image}: {len(items)} imports")
        per_dll = Counter(r["dll"] for r in items)
        for dll, n in sorted(per_dll.items()):
            cats = Counter(r["category"] for r in items if r["dll"] == dll)
            print(f"  {dll:<14} {n:>4}  " + " ".join(f"{c}={k}" for c, k in sorted(cats.items())))
    exe = by_image.get("trespass.exe", [])
    if exe and calls:
        used = [r for r in exe if r["lifted_sites"]]
        print(f"trespass.exe: {len(used)} of {len(exe)} imports are referenced by the lifted code "
              f"({sum(r['lifted_sites'] for r in used)} sites)")
        for cat in sorted(CATEGORY):
            sel = [r for r in exe if r["category"] == cat]
            if sel:
                print(f"  {cat} {CATEGORY[cat]}: {len(sel)} imports, {sum(1 for r in sel if r['lifted_sites'])} called")
    return 0


if __name__ == "__main__":
    sys.exit(main())
