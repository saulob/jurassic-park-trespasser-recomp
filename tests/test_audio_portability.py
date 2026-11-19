"""Static rules that keep the audio and Smacker layers portable (Windows x86 and Linux x86_64).

The portable files (src/host/audio except platform/, src/host/abi, src/host/video) must not name a Windows header,
type, lock or atomic, must not squeeze a pointer into 32 bits and must not load a DLL; the Windows bridge lives under
src/host/audio/platform and is the only audio code that may. The CMake source lists keep the two apart.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIO = ROOT / "src" / "host" / "audio"
ABI = ROOT / "src" / "host" / "abi"
VIDEO = ROOT / "src" / "host" / "video"

FORBIDDEN_INCLUDES = re.compile(
    r'#\s*include\s*[<"][^>"]*\b(windows|dsound|mmsystem|mmreg|objbase|ole2|unknwn|basetsd|winnt|winbase|process|intrin|malloc)\.h[>"]', re.I)
FORBIDDEN_TOKENS = re.compile(
    r"\b(CRITICAL_SECTION|EnterCriticalSection|LeaveCriticalSection|InitializeCriticalSection|DeleteCriticalSection|"
    r"Interlocked\w+|LoadLibrary\w*|FreeLibrary|GetProcAddress|GetModuleHandle\w*|VirtualProtect|VirtualAlloc|"
    r"HRESULT|IUnknown|STDMETHODCALLTYPE|WINAPI|CALLBACK|__stdcall|__declspec|"
    r"DWORD|LPDWORD|LPVOID|LPCVOID|ULONG|LONG|WORD|BYTE|BOOL|HANDLE|HMODULE|HWND|GUID|LPCGUID|REFIID|"
    r"WAVEFORMATEX|DSBUFFERDESC\w*|DSCAPS|DSBCAPS|IDirectSound\w*|PIMAGE_\w+|_aligned_malloc|_aligned_free|_stricmp|_strnicmp)\b")
POINTER_TRUNCATION = re.compile(r"\(\s*(uint32_t|unsigned|unsigned int|int|int32_t|DWORD|ULONG)\s*\)\s*\(\s*(uintptr_t|size_t|intptr_t)\s*\)")
RAW_LONG = re.compile(r"\b(long|unsigned\s+long)\b")
NO_DLL = re.compile(r"\b(dlopen|dlsym|LoadLibrary\w*|wine|proton)\b", re.I)


def strip(text):
    """The code of a C source without comments and string or character literals."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"'(\\.|[^'\\\n])*'", "''", text)
    return text


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def portable_audio_files():
    files = []
    for path in sorted(AUDIO.rglob("*")):
        if path.suffix in (".c", ".h") and "platform" not in path.relative_to(AUDIO).parts:
            files.append(path)
    return files


def portable_files():
    files = portable_audio_files()
    files += sorted(p for p in ABI.rglob("*") if p.suffix in (".c", ".h"))
    files += sorted(p for p in VIDEO.rglob("smacker_*") if p.suffix in (".c", ".h"))
    return files


class PortableAudioLayer(unittest.TestCase):
    def test_the_portable_files_exist(self):
        names = {p.name for p in portable_files()}
        for required in ("dsound_compat.c", "dsound_abi.h", "dsound_methods.h", "audio_mixer.c", "audio_sync.c", "guest_ops.h",
                         "smacker_compat.c", "smacker_backend.h", "smacker_backend_ffmpeg.c"):
            self.assertIn(required, names)

    def test_no_windows_header_is_included(self):
        for path in portable_files():
            for number, line in enumerate(read(path).splitlines(), 1):
                self.assertIsNone(FORBIDDEN_INCLUDES.search(line), f"{path.relative_to(ROOT)}:{number}: {line.strip()}")

    def test_no_windows_type_lock_atomic_or_loader(self):
        for path in portable_files():
            code = strip(read(path))
            found = sorted(set(m.group(0) for m in FORBIDDEN_TOKENS.finditer(code)))
            self.assertEqual(found, [], f"{path.relative_to(ROOT)} uses {found}")

    def test_no_pointer_squeezed_into_32_bits(self):
        for path in portable_files():
            code = strip(read(path))
            self.assertIsNone(POINTER_TRUNCATION.search(code), f"{path.relative_to(ROOT)} casts a pointer-sized integer to 32 bits")

    def test_no_platform_dependent_integer_widths(self):
        for path in portable_files():
            code = strip(read(path))
            self.assertIsNone(RAW_LONG.search(code), f"{path.relative_to(ROOT)} uses long, whose size depends on the platform")

    def test_the_one_lock_is_the_sdl_mutex(self):
        sync = read(AUDIO / "audio_sync.c")
        self.assertIn("SDL_CreateMutex", sync)
        self.assertIn("SDL_LockMutex", sync)
        core = strip(read(AUDIO / "dsound_compat.c"))
        self.assertIn("audio_mutex_lock", core)
        self.assertNotRegex(core, r"\b(pthread_\w+|std::mutex|mtx_\w+)\b")

    def test_smacker_hle_never_loads_a_dll_or_wine(self):
        for path in sorted(VIDEO.rglob("smacker_*")):
            if path.suffix not in (".c", ".h"):
                continue
            code = strip(read(path))
            self.assertIsNone(NO_DLL.search(code), f"{path.relative_to(ROOT)} loads native code or names Wine")

    def test_the_abi_header_pins_its_layout(self):
        abi = read(AUDIO / "dsound_abi.h")
        for name in ("guid_size", "waveformatex_size", "bufferdesc_size", "caps_size", "caps_3d_offset"):
            self.assertIn(name, abi)
        self.assertIn("#pragma pack(push, 1)", abi)
        self.assertIn("stdint.h", abi)

    def test_guest_pointers_are_32_bit_numbers_only_in_the_abi(self):
        guest = read(ABI / "guest_ops.h")
        self.assertIn("uint32_t address", guest)
        self.assertNotRegex(strip(guest), r"\bvoid\s*\*\s*\w+\s*=\s*\(void\s*\*\)\s*\(uintptr_t\)")


class PlatformSeparation(unittest.TestCase):
    def setUp(self):
        self.cmake = read(ROOT / "cmake" / "JptAudio.cmake")

    def block(self, name):
        match = re.search(r"set\(\s*" + name + r"\b(.*?)\)", self.cmake, re.S)
        self.assertIsNotNone(match, f"{name} is not defined in cmake/JptAudio.cmake")
        return match.group(1)

    def test_the_lists_are_separate(self):
        portable = self.block("JPT_AUDIO_PORTABLE_SOURCES")
        windows = self.block("JPT_AUDIO_WINDOWS_SOURCES")
        self.assertNotIn("platform/", portable)
        self.assertIn("platform/windows_dsound_bridge.c", windows)
        self.assertIn("platform/windows_dsound_imports.c", windows)
        self.assertIn("dsound_compat.c", portable)
        self.assertIn("sdl_audio.c", portable)
        self.assertNotIn("windows_", self.block("JPT_AUDIO_LINUX_SOURCES"))
        smacker = self.block("JPT_SMACKER_PORTABLE_SOURCES")
        self.assertIn("smacker_compat.c", smacker)
        self.assertNotIn("platform/", smacker)

    def test_every_file_of_a_list_exists_and_every_portable_file_is_listed(self):
        listed = set()
        for name in ("JPT_AUDIO_PORTABLE_SOURCES", "JPT_AUDIO_WINDOWS_SOURCES", "JPT_SMACKER_PORTABLE_SOURCES"):
            for entry in re.findall(r"\$\{JPT_REPO_ROOT\}/(\S+)", self.block(name)):
                self.assertTrue((ROOT / entry).is_file(), f"{entry} is listed but missing")
                listed.add(entry)
        for path in sorted(AUDIO.rglob("*.c")):
            self.assertIn(path.relative_to(ROOT).as_posix(), listed, f"{path.name} is in no source list")
        for path in sorted(VIDEO.rglob("smacker_*.c")):
            self.assertIn(path.relative_to(ROOT).as_posix(), listed, f"{path.name} is in no source list")

    def test_the_windows_bridge_is_the_only_audio_code_with_windows_headers(self):
        for path in sorted(AUDIO.rglob("*.c")) + sorted(AUDIO.rglob("*.h")):
            text = read(path)
            has_windows = re.search(r"#\s*include\s*<(windows|dsound|mmsystem|mmreg)\.h>", text) is not None
            in_platform = "platform" in path.relative_to(AUDIO).parts
            if has_windows:
                self.assertTrue(in_platform and path.name.startswith("windows_"), f"{path.relative_to(ROOT)} includes a Windows header")

    def test_the_game_build_takes_the_lists_from_the_module(self):
        cmake = read(ROOT / "CMakeLists.txt")
        self.assertIn("include(JptAudio)", cmake)
        self.assertIn("JPT_AUDIO_PORTABLE_SOURCES", cmake)
        self.assertIn("JPT_AUDIO_PLATFORM_SOURCES", cmake)
        host = re.search(r"set\(HOST_SOURCES(.*?)\n\)", cmake, re.S)
        self.assertIsNotNone(host)
        self.assertNotIn("src/host/audio/", host.group(1))
        self.assertNotRegex(cmake, r"dsound\.lib|target_link_libraries\(trespasser_recomp PRIVATE dsound")

    def test_the_linux_probe_builds_the_same_lists(self):
        probe = read(ROOT / "tests" / "linux" / "CMakeLists.txt")
        self.assertIn("include(JptAudio)", probe)
        self.assertIn("JPT_AUDIO_PORTABLE_SOURCES", probe)
        self.assertIn("JPT_SMACKER_PORTABLE_SOURCES", probe)
        self.assertIn("UNIX AND NOT APPLE", probe)
        self.assertNotIn("JPT_AUDIO_WINDOWS_SOURCES", probe)


if __name__ == "__main__":
    unittest.main()
