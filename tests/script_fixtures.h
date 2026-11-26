// What the script session suites share (script_session_tests.cpp, script_save_tests.cpp): a log, an allocator that counts
// what is out, the package's scripts in memory, sessions over them, packages and saves on disk, and the scripts they run.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "jpt_custom_level.h"
#include "jpt_level_scripts.h"
#include "jpt_save.h"
#include "jpt_script_heap.h"
#include "jpt_script_session.h"
#include "jpt_script_state.h"
#include "level_fixtures.h"

namespace fs = std::filesystem;

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static std::vector<std::string> g_lines;

static inline void Capture(void*, const char* line) { g_lines.push_back(line); }
static inline void CaptureLine(const char* line) { g_lines.push_back(line); }

static inline int Find(const std::string& line) {
    for (size_t i = 0; i < g_lines.size(); ++i)
        if (g_lines[i] == line) return static_cast<int>(i);
    return -1;
}

static inline bool Has(const std::string& line) { return Find(line) >= 0; }

static inline bool Contains(const std::string& part) {
    for (const std::string& l : g_lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

static inline int Count(const std::string& prefix) {
    int n = 0;
    for (const std::string& l : g_lines)
        if (!l.compare(0, prefix.size(), prefix)) n++;
    return n;
}

static inline bool InOrder(std::initializer_list<const char*> lines) {
    int last = -1;
    for (const char* line : lines) {
        const int at = Find(line);
        if (at <= last) {
            printf("  not in order: %s\n", line);
            return false;
        }
        last = at;
    }
    return true;
}

static inline std::string Print(const std::string& text) { return "[script] print: " + text; }

// The system allocator under every VM of these tests: what is still out.
static std::unordered_map<void*, size_t> g_blocks;
static size_t g_bytes;

static inline void* FakeAllocate(size_t size) {
    void* block = malloc(size ? size : 1);
    g_blocks[block] = size;
    g_bytes += size;
    return block;
}

static inline void* FakeReallocate(void* block, size_t size) {
    const size_t old = g_blocks[block];
    void* moved = realloc(block, size ? size : 1);
    if (!moved) return nullptr;
    g_blocks.erase(block);
    g_blocks[moved] = size;
    g_bytes = g_bytes - old + size;
    return moved;
}

static inline void FakeRelease(void* block) {
    g_bytes -= g_blocks[block];
    g_blocks.erase(block);
    free(block);
}

static const jpt_script_allocator kFake = {FakeAllocate, FakeReallocate, FakeRelease};

// The package's scripts as the session reads them; a path in g_fail_after fails from that read on.
static std::map<std::string, std::string> g_files;
static std::map<std::string, int> g_reads, g_fail_after;

static inline char* Read(void*, const char* path, size_t* size) {
    const auto it = g_files.find(path);
    const int reads = ++g_reads[path];
    const auto fail = g_fail_after.find(path);
    if (it == g_files.end() || (fail != g_fail_after.end() && reads > fail->second)) return nullptr;
    char* bytes = static_cast<char*>(malloc(it->second.size() + 1));
    memcpy(bytes, it->second.data(), it->second.size());
    *size = it->second.size();
    return bytes;
}

using Files = std::vector<std::pair<std::string, std::string>>;

static inline jpt_script_session* Session(const Files& files, size_t memory = 0, long instructions = 200000) {
    g_lines.clear();
    g_files.clear();
    g_reads.clear();
    g_fail_after.clear();
    jpt_script_session_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.read = Read;
    config.instructions = instructions;
    config.memory_limit = memory;
    config.allocator = &kFake;
    jpt_script_session* s = jpt_script_session_create(&config);
    for (const auto& [path, text] : files) {
        g_files[path] = text;
        jpt_script_session_add_file(s, path.c_str());
    }
    return s;
}

static inline void Close(jpt_script_session* s) {
    jpt_script_session_destroy(s);
    CHECK(g_bytes == 0 && g_blocks.empty());
    CHECK(!Contains("still held"));
}

static inline size_t Held(const jpt_script_session* s) {
    jpt_script_memory m;
    memset(&m, 0, sizeof m);
    return jpt_script_session_memory(s, &m) ? m.current : 0;
}

static inline void Begin(jpt_script_session* s, const char* scene, jpt_script_begin why, const std::vector<uint8_t>* state = nullptr) {
    jpt_script_level_begin(s, scene, why, state ? state->data() : nullptr, state ? state->size() : 0);
}

static inline void Steps(jpt_script_session* s, int n) {
    for (int i = 0; i < n; ++i) jpt_script_level_step(s, 0.1f);
}

static inline std::vector<uint8_t> SaveOf(jpt_script_session* s) {
    size_t size = 0;
    uint8_t* bytes = jpt_script_session_save(s, &size);
    std::vector<uint8_t> out(bytes, bytes + size);
    free(bytes);
    return out;
}

static const char* kGame =
    "class Campaign extends GameScript {\n"
    "  nvram = null;\n"
    "  constructor() { nvram = { levels = 0, started = false, steps = 0 }; print(\"game made\"); }\n"
    "  function OnScriptInit(bSaveGame) { print(\"game init \" + bSaveGame + \" levels=\" + nvram.levels + \" steps=\" + nvram.steps); }\n"
    "  function OnGameStart() { nvram.started = true; print(\"game start\"); }\n"
    "  function OnLevelStart() { nvram.levels++; print(\"game level start levels=\" + nvram.levels); }\n"
    "  function OnLevelEnd(bNext) { print(\"game end \" + bNext); }\n"
    "  function OnSimStep(dt) { nvram.steps++; }\n"
    "  function OnPrepareSave() { print(\"game prepare save\"); }\n"
    "  function OnWorldPurge() { print(\"game purge\"); }\n"
    "  function destructor() { print(\"game gone\"); }\n"
    "}\n";

static const char* kShared =
    "class AlphaRules extends LevelScript {\n"
    "  static SCN = \"alpha.scn\";\n"
    "  nvram = null;\n"
    "  constructor() { nvram = { steps = 0 }; print(\"alpha made\"); }\n"
    "  function OnScriptInit(bSaveGame) { print(\"alpha init \" + bSaveGame + \" steps=\" + nvram.steps); }\n"
    "  function OnLevelStart() { print(\"alpha start steps=\" + nvram.steps); }\n"
    "  function OnSimStep(dt) { nvram.steps++; }\n"
    "  function OnLevelEnd(bNext) { print(\"alpha end \" + bNext); }\n"
    "  function OnPrepareSave() { print(\"alpha prepare save\"); }\n"
    "  function OnWorldPurge() { print(\"alpha purge\"); }\n"
    "  function destructor() { print(\"alpha gone\"); }\n"
    "}\n";

static inline fs::path Folder() {
    const fs::path dir = fs::temp_directory_path() / "jpt_script_session_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "custom_levels" / "Pkg" / "scripts");
    return dir;
}

static inline void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static inline std::string Slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static size_t g_state_bytes;
static inline size_t SomeState(uint8_t* out, size_t cap) { return g_state_bytes <= cap ? (memset(out, 'L', g_state_bytes), g_state_bytes) : 0; }

// A save written as the game's: its own file first, then the stamp, the actions' state and the scripts' block.
static inline bool Saved(const fs::path& dir, const char* name, const char* package) {
    const fs::path temp = dir / "~save.tmp";
    char problem[128];
    Write(temp, fixtures::Save("alpha.grf", "alpha.scn"));
    return jpt_save_finish((dir / name).string().c_str(), temp.string().c_str(), package, nullptr, problem, sizeof problem) != 0;
}
