#include "jpt_script_session.h"

#include "jpt_script_plan.h"
#include "jpt_script_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using jpt::script::Entry;
using jpt::script::Kind;
using jpt::script::Role;
using jpt::script::Slot;

struct Script {
    std::string name;
    Kind kind;
    int handle;
    bool step;
};

using Nvram = std::map<std::string, std::vector<Slot>>;

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string BaseName(const char* path) {
    const char* slash = path ? strrchr(path, '/') : nullptr;
    const char* back = path ? strrchr(path, '\\') : nullptr;
    if (back && (!slash || back > slash)) slash = back;
    return slash ? slash + 1 : path ? path : "";
}

}  // namespace

struct jpt_script_session {
    jpt_script_session_config config{};
    std::vector<std::string> paths;
    std::vector<std::pair<std::string, jpt::script::WorldScript>> references; // by scene, lower case
    std::vector<std::pair<std::string, std::string>> level_files;            // by scene, lower case
    bool per_level = false;
    std::map<std::string, jpt::script::FileInfo> scanned;
    jpt_script_vm* vm = nullptr;
    std::vector<Script> scripts;
    std::string level;
    jpt_script_begin why = JPT_SCRIPT_BEGIN_NEW_GAME;
    bool pending = false;
    unsigned generation = 0;
    Nvram game;              // the game scripts' nvram outside the VM
    Nvram level_start;       // theirs before OnLevelStart, for Restart Level
    std::string start_scene; // the level that snapshot is of
    std::vector<Entry> staged; // a save's level scripts, until they are created
    std::set<std::string> noted;
};

namespace {

void Say(jpt_script_session* s, const char* format, ...) {
    if (!s->config.log) return;
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    s->config.log(s->config.user, line);
}

const jpt::script::FileInfo& Scan(jpt_script_session* s, const std::string& path) {
    const auto known = s->scanned.find(path);
    if (known != s->scanned.end()) return known->second;
    size_t size = 0;
    char* bytes = s->config.read ? s->config.read(s->config.user, path.c_str(), &size) : nullptr;
    jpt::script::FileInfo info;
    if (bytes) {
        info = jpt::script::ScanFile(path, std::string_view(bytes, size));
        free(bytes);
    } else {
        info.path = path;
        info.problem = "it cannot be read";
    }
    return s->scanned[path] = info;
}

// The scripts that may concern a level: the ones the launcher found for it, or every script of the package.
std::vector<jpt::script::FileInfo> Candidates(jpt_script_session* s) {
    std::vector<jpt::script::FileInfo> files;
    if (!s->per_level) {
        for (const std::string& path : s->paths) files.push_back(Scan(s, path));
        return files;
    }
    for (const auto& [scene, path] : s->level_files)
        if (scene == Lower(s->level)) files.push_back(Scan(s, path));
    return files;
}

void Each(jpt_script_session* s, Kind kind, const char* method, const jpt_script_arg* args, int count) {
    for (const Script& script : s->scripts)
        if (script.kind == kind) jpt_script_vm_invoke(s->vm, script.handle, script.name.c_str(), method, args, count);
}

void Both(jpt_script_session* s, const char* method, const jpt_script_arg* args = nullptr, int count = 0) {
    Each(s, Kind::kGame, method, args, count);
    Each(s, Kind::kLevel, method, args, count);
}

void Note(jpt_script_session* s, const std::string& script, const std::vector<std::string>& notes) {
    for (const std::string& note : notes)
        if (s->noted.insert(script + "|" + note).second)
            Say(s, "[script] level=%s script=%s nvram slot not saved: %s", s->level.c_str(), script.c_str(), note.c_str());
}

// A VM abandoned for its memory has nothing left to read: what was kept before stays.
bool Alive(const jpt_script_session* s) {
    jpt_script_memory memory{};
    return s->vm && jpt_script_vm_memory(s->vm, &memory) && !memory.abandoned;
}

Nvram Capture(jpt_script_session* s, Kind kind) {
    Nvram out;
    for (const Script& script : s->scripts) {
        std::vector<Slot> slots;
        std::vector<std::string> notes;
        if (script.kind != kind || !jpt_script_vm_get_nvram(s->vm, script.handle, slots, notes)) continue;
        Note(s, script.name, notes);
        out[script.name] = slots;
    }
    return out;
}

void EndVm(jpt_script_session* s) {
    if (!s->vm) return;
    Both(s, "OnWorldPurge");
    Both(s, "destructor");
    jpt_script_memory memory{};
    jpt_script_vm_memory(s->vm, &memory);
    jpt_script_vm_destroy(s->vm);
    s->vm = nullptr;
    s->scripts.clear();
    s->pending = false;
    Say(s, "[script] vm destroy level=%s generation=%u peak=%zu abandoned=%d", s->level.c_str(), s->generation, memory.peak,
        memory.abandoned);
}

void Restore(jpt_script_session* s, const Script& script, const std::vector<Slot>& slots) {
    if (!jpt_script_vm_set_nvram(s->vm, script.handle, slots))
        Say(s, "[script] level=%s script=%s nvram not restored: the script has no nvram table", s->level.c_str(), script.name.c_str());
}

void Stage(jpt_script_session* s, const uint8_t* state, size_t size) {
    std::vector<Entry> entries;
    std::string problem;
    s->game.clear();
    s->level_start.clear();
    s->staged.clear();
    s->start_scene = s->level;
    if (!state || !size) return;
    if (!jpt::script::DecodeState(state, size, entries, problem)) {
        Say(s, "[script] level=%s save state ignored: %s; the scripts start from their constructors", s->level.c_str(), problem.c_str());
        return;
    }
    for (const Entry& e : entries) {
        if (e.kind == static_cast<uint8_t>(Kind::kGame) && e.role == Role::kCurrent) s->game[e.class_name] = e.slots;
        else if (e.kind == static_cast<uint8_t>(Kind::kGame) && e.role == Role::kLevelStart) s->level_start[e.class_name] = e.slots;
        else if (e.role == Role::kCurrent) s->staged.push_back(e);
    }
}

void CreateVm(jpt_script_session* s, const jpt::script::LevelPlan& plan) {
    jpt_script_config config{};
    config.log = s->config.log;
    config.user = s->config.user;
    config.instructions = s->config.instructions;
    config.memory_limit = s->config.memory_limit;
    config.allocator = s->config.allocator;
    s->vm = jpt_script_vm_create(&config);
    if (!s->vm) {
        Say(s, "[script] level=%s result=vm_disabled error=the VM could not open within its memory limit", s->level.c_str());
        return;
    }
    jpt_script_vm_set_level(s->vm, s->level.c_str());
    jpt_script_vm_define_class(s->vm, "GameScript");
    jpt_script_vm_define_class(s->vm, "LevelScript");
    s->generation++;
    jpt_script_memory memory{};
    jpt_script_vm_memory(s->vm, &memory);
    Say(s, "[script] vm create level=%s generation=%u scripts=%zu files=%zu memory_limit=%zu", s->level.c_str(), s->generation,
        plan.classes.size(), plan.files.size(), memory.limit);
    for (const std::string& path : plan.files) {
        size_t size = 0;
        char* bytes = s->config.read ? s->config.read(s->config.user, path.c_str(), &size) : nullptr;
        if (!bytes) {
            Say(s, "[script] level=%s file=%s callback=load result=missing", s->level.c_str(), path.c_str());
            continue;
        }
        if (jpt_script_vm_load(s->vm, path.c_str(), bytes, size) == JPT_SCRIPT_OK) Say(s, "[script] loaded file=%s", path.c_str());
        free(bytes);
    }
    for (const jpt::script::PlannedClass& p : plan.classes) {
        const char* base = p.kind == Kind::kGame ? "GameScript" : "LevelScript";
        if (p.kind != Kind::kGame && p.kind != Kind::kLevel) continue;
        if (!jpt_script_vm_extends(s->vm, p.name.c_str(), base)) {
            Say(s, "[script] level=%s script=%s result=disabled error=it is not a class derived from %s once loaded", s->level.c_str(),
                p.name.c_str(), base);
            continue;
        }
        const int handle = jpt_script_vm_instantiate(s->vm, p.name.c_str(), p.name.c_str());
        if (handle) s->scripts.push_back({p.name, p.kind, handle, p.sim_step && jpt_script_vm_has_method(s->vm, handle, "OnSimStep")});
    }
}

void RestoreAll(jpt_script_session* s) {
    for (const Script& script : s->scripts) {
        if (script.kind != Kind::kGame) continue;
        const auto it = s->game.find(script.name);
        if (it != s->game.end()) Restore(s, script, it->second);
    }
    for (const auto& [name, slots] : s->game) {
        const bool there = std::any_of(s->scripts.begin(), s->scripts.end(), [&name](const Script& x) { return x.kind == Kind::kGame && x.name == name; });
        if (!there && s->why == JPT_SCRIPT_BEGIN_SAVE)
            Say(s, "[script] level=%s save state of script=%s skipped: the level has no such game script now", s->level.c_str(), name.c_str());
    }
    for (const Entry& e : s->staged) {
        const auto it = std::find_if(s->scripts.begin(), s->scripts.end(), [&e](const Script& x) {
            return x.kind == Kind::kLevel && e.kind == static_cast<uint8_t>(Kind::kLevel) && x.name == e.class_name;
        });
        if (it == s->scripts.end())
            Say(s, "[script] level=%s save state of script=%s skipped: the level has no such %s script now", s->level.c_str(),
                e.class_name.c_str(), jpt::script::KindName(static_cast<Kind>(e.kind)));
        else
            Restore(s, *it, e.slots);
    }
    s->staged.clear();
}

}  // namespace

extern "C" jpt_script_session* jpt_script_session_create(const jpt_script_session_config* config) {
    jpt_script_session* s = new jpt_script_session;
    if (config) s->config = *config;
    s->per_level = s->config.per_level_files != 0;
    return s;
}

extern "C" void jpt_script_session_destroy(jpt_script_session* s) {
    if (!s) return;
    EndVm(s);
    if (s->generation) Say(s, "[script] session end vms=%u", s->generation);
    delete s;
}

extern "C" void jpt_script_session_add_file(jpt_script_session* s, const char* path) {
    if (!s || !path || !*path) return;
    s->paths.push_back(path);
}

extern "C" void jpt_script_session_add_level_file(jpt_script_session* s, const char* scene, const char* path) {
    if (!s || !scene || !path || !*path) return;
    s->per_level = true;
    s->level_files.push_back({Lower(BaseName(scene)), path});
}

extern "C" void jpt_script_session_add_reference(jpt_script_session* s, const char* scene, const char* class_name, const char* object, int action) {
    if (!s || !scene || !class_name || !*class_name) return;
    s->references.push_back({Lower(BaseName(scene)), {class_name, object ? object : "", action != 0}});
}

extern "C" void jpt_script_level_begin(jpt_script_session* s, const char* scene, jpt_script_begin why, const uint8_t* state, size_t size) {
    if (!s) return;
    if (s->vm && why == JPT_SCRIPT_BEGIN_TRANSITION && !s->pending) {
        const jpt_script_arg leaving{JPT_SCRIPT_ARG_BOOL, 1, 0.0f, nullptr};
        Both(s, "OnLevelEnd", &leaving, 1);
        Say(s, "[script] callback level_end level=%s scripts=%zu", s->level.c_str(), s->scripts.size());
        if (Alive(s)) s->game = Capture(s, Kind::kGame);
    }
    EndVm(s);
    s->level = BaseName(scene);
    s->why = why;
    if (why == JPT_SCRIPT_BEGIN_NEW_GAME) {
        s->game.clear();
        s->level_start.clear();
        s->start_scene.clear();
    } else if (why == JPT_SCRIPT_BEGIN_RESTART && Lower(s->start_scene) == Lower(s->level)) {
        s->game = s->level_start;
    } else if (why == JPT_SCRIPT_BEGIN_SAVE) {
        Stage(s, state, size);
    }
    std::vector<jpt::script::WorldScript> world;
    for (const auto& [where, ref] : s->references)
        if (where == Lower(s->level)) world.push_back(ref);
    const std::vector<jpt::script::FileInfo> files = Candidates(s);
    const jpt::script::LevelPlan plan = jpt::script::PlanLevel(files, s->level, world);
    if (!plan.needs_scripts()) {
        if (!s->staged.empty()) Say(s, "[script] level=%s save state of %zu level scripts skipped: the level needs no script now", s->level.c_str(), s->staged.size());
        s->staged.clear();
        return;
    }
    if (!plan.supported()) {
        std::string names;
        for (const std::string& name : plan.required) names += (names.empty() ? "" : ",") + name;
        for (const std::string& problem : plan.problems) names += (names.empty() ? "" : ",") + problem;
        Say(s, "[script] level=%s vm=none result=refused error=its scripts need what the port does not have: %s", s->level.c_str(), names.c_str());
        return;
    }
    CreateVm(s, plan);
    if (!s->vm) return;
    RestoreAll(s);
    s->pending = true;
}

extern "C" void jpt_script_level_ready(jpt_script_session* s, int ok) {
    if (!s || !s->vm || !s->pending) return;
    s->pending = false;
    if (!ok) {
        Say(s, "[script] level=%s the game's load failed: its scripts stop", s->level.c_str());
        EndVm(s);
        return;
    }
    const bool save = s->why == JPT_SCRIPT_BEGIN_SAVE;
    const jpt_script_arg game_init{JPT_SCRIPT_ARG_BOOL, save || s->why == JPT_SCRIPT_BEGIN_TRANSITION, 0.0f, nullptr};
    const jpt_script_arg level_init{JPT_SCRIPT_ARG_BOOL, save, 0.0f, nullptr};
    Each(s, Kind::kGame, "OnScriptInit", &game_init, 1);
    Each(s, Kind::kLevel, "OnScriptInit", &level_init, 1);
    if (s->why == JPT_SCRIPT_BEGIN_NEW_GAME) Each(s, Kind::kGame, "OnGameStart", nullptr, 0);
    if (save) {
        Say(s, "[script] callback script_init level=%s save=1 scripts=%zu", s->level.c_str(), s->scripts.size());
        return;
    }
    if (!(s->why == JPT_SCRIPT_BEGIN_RESTART && Lower(s->start_scene) == Lower(s->level)) && Alive(s)) {
        s->level_start = Capture(s, Kind::kGame);
        s->start_scene = s->level;
    }
    Both(s, "OnLevelStart");
    Say(s, "[script] callback level_start level=%s scripts=%zu", s->level.c_str(), s->scripts.size());
}

extern "C" void jpt_script_level_step(jpt_script_session* s, float seconds) {
    if (!s || !s->vm || s->pending || !(seconds > 0.0f)) return;
    const jpt_script_arg step{JPT_SCRIPT_ARG_FLOAT, 0, seconds, nullptr};
    for (const Script& script : s->scripts)
        if (script.step) jpt_script_vm_invoke(s->vm, script.handle, script.name.c_str(), "OnSimStep", &step, 1);
}

extern "C" uint8_t* jpt_script_session_save(jpt_script_session* s, size_t* size) {
    if (size) *size = 0;
    if (!s || !size) return nullptr;
    Nvram levels;
    if (Alive(s) && !s->pending) {
        Both(s, "OnPrepareSave");
        s->game = Capture(s, Kind::kGame);
        levels = Capture(s, Kind::kLevel);
    }
    std::vector<Entry> entries;
    const auto add = [&entries](const Nvram& from, Role role, Kind kind) {
        for (const auto& [name, slots] : from) entries.push_back({role, static_cast<uint8_t>(kind), name, "", slots});
    };
    add(s->game, Role::kCurrent, Kind::kGame);
    add(levels, Role::kCurrent, Kind::kLevel);
    if (Lower(s->start_scene) == Lower(s->level)) add(s->level_start, Role::kLevelStart, Kind::kGame);
    if (entries.empty()) return nullptr;
    const std::vector<uint8_t> bytes = jpt::script::EncodeState(entries);
    if (bytes.size() > jpt::script::kStateMax) {
        Say(s, "[script] level=%s save state not kept: %zu bytes, past %zu", s->level.c_str(), bytes.size(), jpt::script::kStateMax);
        return nullptr;
    }
    uint8_t* out = static_cast<uint8_t*>(malloc(bytes.size()));
    if (!out) return nullptr;
    memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    Say(s, "[script] save level=%s scripts=%zu bytes=%zu", s->level.c_str(), entries.size(), bytes.size());
    return out;
}

extern "C" int jpt_script_session_has_vm(const jpt_script_session* s) { return s && s->vm; }

extern "C" unsigned jpt_script_session_generation(const jpt_script_session* s) { return s ? s->generation : 0; }

extern "C" int jpt_script_session_memory(const jpt_script_session* s, jpt_script_memory* out) {
    return s && s->vm && jpt_script_vm_memory(s->vm, out);
}

extern "C" int jpt_script_session_scripts(const jpt_script_session* s, int* stepping) {
    if (stepping) *stepping = 0;
    if (!s) return 0;
    if (stepping) *stepping = static_cast<int>(std::count_if(s->scripts.begin(), s->scripts.end(), [](const Script& x) { return x.step; }));
    return static_cast<int>(s->scripts.size());
}
