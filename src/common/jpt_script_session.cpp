#include "jpt_script_session.h"

#include "jpt_script_engine.h"
#include "jpt_script_plan.h"
#include "jpt_script_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
    Kind kind = Kind::kGame;
    int handle = 0;
    bool step = false;
    std::string object;  // an instance script's object
    uint32_t entity = 0; // its unique handle
    bool paint = false, input = false;
    bool speed = false, jump_factor = false, on_jump = false, console = false;
};

Script NewScript(const std::string& name, Kind kind, int handle) {
    Script script;
    script.name = name;
    script.kind = kind;
    script.handle = handle;
    return script;
}

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
    std::vector<Entry> staged; // a save's level and instance scripts, until they are created
    jpt::script::LevelPlan plan;
    jpt_draw_frame_t frame{};
    std::vector<jpt_draw_texture_t> textures;
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

void All(jpt_script_session* s, const char* method) {
    Each(s, Kind::kInstance, method, nullptr, 0);
    Both(s, method);
}

std::string OwnerOf(const std::string& class_name, const std::string& object) { return class_name + ":" + object; }

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

// By class; an instance script's by "class:object".
Nvram Capture(jpt_script_session* s, Kind kind) {
    Nvram out;
    for (const Script& script : s->scripts) {
        std::vector<Slot> slots;
        std::vector<std::string> notes;
        if (script.kind != kind || !jpt_script_vm_get_nvram(s->vm, script.handle, slots, notes)) continue;
        const std::string key = kind == Kind::kInstance ? OwnerOf(script.name, script.object) : script.name;
        Note(s, key, notes);
        out[key] = slots;
    }
    return out;
}

void EndVm(jpt_script_session* s) {
    if (!s->vm) return;
    Both(s, "OnWorldPurge");
    All(s, "destructor");
    jpt_script_memory memory{};
    jpt_script_vm_memory(s->vm, &memory);
    jpt_script_vm_destroy(s->vm);
    s->vm = nullptr;
    s->scripts.clear();
    s->frame = jpt_draw_frame_t{};
    s->textures.clear();
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
    config.engine = s->config.engine;
    config.random_seed = s->config.random_seed;
    s->vm = jpt_script_vm_create(&config);
    if (!s->vm) {
        Say(s, "[script] level=%s result=vm_disabled error=the VM could not open within its memory limit", s->level.c_str());
        return;
    }
    jpt_script_vm_set_level(s->vm, s->level.c_str());
    s->generation++;
    jpt_script_vm_set_world(s->vm, s->generation, 0);
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
        if (!handle) continue;
        jpt_script_vm_set_receiver(s->vm, handle);
        const bool game = p.kind == Kind::kGame;
        Script script = NewScript(p.name, p.kind, handle);
        script.step = p.sim_step && jpt_script_vm_has_method(s->vm, handle, "OnSimStep");
        script.paint = game && jpt_script_vm_has_method(s->vm, handle, "OnPaintHW3D") != 0;
        script.input = game && jpt_script_vm_has_method(s->vm, handle, "OnHandleInput") != 0;
        script.console = game && jpt_script_vm_has_method(s->vm, handle, "OnConsoleCommand") != 0;
        s->scripts.push_back(script);
    }
}

// The world's objects the level's instance scripts are for, each checked against its class's engine class.
void AttachInstances(jpt_script_session* s) {
    const jpt_script_engine* engine = s->config.engine;
    for (const jpt::script::PlannedClass& p : s->plan.classes) {
        if (p.kind != Kind::kInstance) continue;
        const std::string base = jpt_script_vm_native_base(s->vm, p.name.c_str());
        for (const std::string& object : p.objects) {
            const std::string owner = OwnerOf(p.name, object);
            const uint32_t entity = engine ? engine->find(engine->user, object.c_str()) : 0;
            const char* native = entity ? engine->native_type(engine->user, entity) : nullptr;
            if (!native) {
                Say(s, "[script] level=%s script=%s result=missing error=the world has no object of that name", s->level.c_str(), owner.c_str());
                continue;
            }
            if (base != native) {
                Say(s, "[script] level=%s script=%s result=disabled error=the class extends %s, the object is a %s", s->level.c_str(),
                    owner.c_str(), base.empty() ? "no engine class" : base.c_str(), native);
                continue;
            }
            const int handle = jpt_script_vm_attach(s->vm, owner.c_str(), p.name.c_str(), entity);
            if (!handle) continue;
            Script script = NewScript(p.name, Kind::kInstance, handle);
            script.step = jpt_script_vm_has_method(s->vm, handle, "OnSimStep") != 0;
            script.object = object;
            script.entity = entity;
            if (base == "CPlayer") {
                script.speed = jpt_script_vm_has_method(s->vm, handle, "fGetSpeedFactor") != 0;
                script.jump_factor = jpt_script_vm_has_method(s->vm, handle, "fGetJumpFactor") != 0;
                script.on_jump = jpt_script_vm_has_method(s->vm, handle, "OnJump") != 0;
            }
            s->scripts.push_back(script);
        }
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
            return static_cast<uint8_t>(x.kind) == e.kind && x.name == e.class_name &&
                   (x.kind == Kind::kLevel || (x.kind == Kind::kInstance && x.object == e.object));
        });
        if (it == s->scripts.end())
            Say(s, "[script] level=%s save state of script=%s skipped: the level has no such %s script now", s->level.c_str(),
                (e.object.empty() ? e.class_name : OwnerOf(e.class_name, e.object)).c_str(), jpt::script::KindName(static_cast<Kind>(e.kind)));
        else {
            Restore(s, *it, e.slots);
            if (it->kind == Kind::kInstance) jpt_script_vm_set_stepping(s->vm, it->entity, (e.flags & jpt::script::kEntrySimStep) != 0);
        }
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
        jpt_script_arg leaving{};
        leaving.type = JPT_SCRIPT_ARG_BOOL;
        leaving.b = 1;
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
    s->plan = plan;
    CreateVm(s, plan);
    if (!s->vm) return;
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
    jpt_script_vm_set_world(s->vm, s->generation, 1);
    AttachInstances(s);
    RestoreAll(s);
    if (save) Each(s, Kind::kInstance, "OnPostLoad", nullptr, 0);
    jpt_script_arg game_init{};
    game_init.type = JPT_SCRIPT_ARG_BOOL;
    game_init.b = save || s->why == JPT_SCRIPT_BEGIN_TRANSITION;
    jpt_script_arg level_init = game_init;
    level_init.b = save;
    Each(s, Kind::kInstance, "OnScriptInit", &level_init, 1);
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
    jpt_script_arg step{};
    step.type = JPT_SCRIPT_ARG_FLOAT;
    step.f = seconds;
    for (const Script& script : s->scripts) {
        if (!script.step) continue;
        if (script.kind != Kind::kInstance) jpt_script_vm_invoke(s->vm, script.handle, script.name.c_str(), "OnSimStep", &step, 1);
        else if (jpt_script_vm_stepping(s->vm, script.entity))
            jpt_script_vm_invoke(s->vm, script.handle, OwnerOf(script.name, script.object).c_str(), "OnSimStep", &step, 1);
    }
}

namespace {

const Script* ScriptOf(const jpt_script_session* s, int handle) {
    for (const Script& script : s->scripts)
        if (script.handle == handle) return &script;
    return nullptr;
}

// Modal mode never outlives the script that holds it: stopped, or gone with its VM.
void ReleaseDeadModal(jpt_script_session* s) {
    const int modal = jpt_script_vm_modal(s->vm);
    const Script* owner = modal ? ScriptOf(s, modal) : nullptr;
    if (modal && (!owner || jpt_script_vm_disabled(s->vm, owner->name.c_str()))) {
        jpt_script_vm_release_modal(s->vm);
        Say(s, "[script] level=%s modal mode released: its script stopped", s->level.c_str());
    }
}

void AbortModal(jpt_script_session* s, const char* why) {
    const Script* owner = ScriptOf(s, jpt_script_vm_modal(s->vm));
    jpt_script_vm_release_modal(s->vm);
    if (!owner) return;
    Say(s, "[script] level=%s script=%s modal mode aborted: %s", s->level.c_str(), owner->name.c_str(), why);
    jpt_script_vm_invoke(s->vm, owner->handle, owner->name.c_str(), "OnModalAborted", nullptr, 0);
}

bool PlayerDead(const jpt_script_session* s) {
    const jpt_script_engine* e = s->config.engine;
    float hit_points = 0.0f;
    int dead = 0;
    const uint32_t player = e && e->player ? e->player(e->user) : 0;
    return player && e->health && e->health(e->user, player, &hit_points, &dead) && dead;
}

}  // namespace

extern "C" const jpt_draw_frame_t* jpt_script_level_paint(jpt_script_session* s) {
    if (!s) return nullptr;
    s->frame = jpt_draw_frame_t{};
    s->textures.clear();
    if (!Alive(s) || s->pending) return &s->frame;
    ReleaseDeadModal(s);
    jpt_script_vm_begin_paint(s->vm);
    for (const Script& script : s->scripts) {
        if (!script.paint) continue;
        const jpt_script_result result = jpt_script_vm_invoke(s->vm, script.handle, script.name.c_str(), "OnPaintHW3D", nullptr, 0);
        if (result == JPT_SCRIPT_FAILED && Alive(s)) jpt_script_vm_drop_draws(s->vm, script.handle);
    }
    jpt_script_vm_end_paint(s->vm);
    if (!Alive(s)) return &s->frame;
    const std::vector<jpt_draw_cmd_t>& draws = jpt_script_vm_draw_list(s->vm);
    for (size_t id = 1; id <= jpt_script_vm_texture_count(s->vm); ++id) {
        jpt_draw_texture_t t{};
        if (jpt_script_vm_texture(s->vm, static_cast<uint32_t>(id), &t)) s->textures.push_back(t);
    }
    const jpt_script_engine* e = s->config.engine;
    int w = 0, h = 0;
    if (!e || !e->screen_size || !e->screen_size(e->user, &w, &h) || w <= 0 || h <= 0) return &s->frame;
    s->frame = {s->generation, w, h, draws.data(), static_cast<int>(draws.size()), s->textures.data(), static_cast<int>(s->textures.size())};
    return &s->frame;
}

extern "C" const jpt_draw_frame_t* jpt_script_level_paint_frame(const jpt_script_session* s) { return s ? &s->frame : nullptr; }

extern "C" void jpt_script_level_input(jpt_script_session* s, const jpt_script_input* in, jpt_script_input_result* out) {
    if (out) *out = jpt_script_input_result{};
    if (!s || !in || !Alive(s) || s->pending) return;
    ReleaseDeadModal(s);
    if (jpt_script_vm_modal(s->vm) && PlayerDead(s)) AbortModal(s, "the player died");
    const uint32_t mask = s->plan.user_commands;
    jpt_script_vm_set_input(s->vm, 1, in->user_held & mask, in->user_hit & mask, in->move_x, in->move_y);
    const int modal = jpt_script_vm_modal(s->vm);
    bool look = false;
    if (modal) {
        const Script* owner = ScriptOf(s, modal);
        double allow = 0.0;
        int has = 0;
        if (owner) jpt_script_vm_invoke_number(s->vm, owner->handle, owner->name.c_str(), "OnHandleModalInput", nullptr, 0, &allow, &has);
        look = has && allow != 0.0;
    } else {
        for (const Script& script : s->scripts)
            if (script.input) jpt_script_vm_invoke(s->vm, script.handle, script.name.c_str(), "OnHandleInput", nullptr, 0);
    }
    if (!Alive(s)) return;
    jpt_script_vm_set_input(s->vm, 0, 0, 0, 0.0f, 0.0f);
    ReleaseDeadModal(s);
    if (out && jpt_script_vm_modal(s->vm)) {
        out->modal = 1;
        out->look = look;
    }
}

extern "C" int jpt_script_level_key(jpt_script_session* s, unsigned vk, int down, int shift, int ctrl, int alt) {
    if (!s || !Alive(s) || s->pending) return 0;
    ReleaseDeadModal(s);
    const Script* owner = ScriptOf(s, jpt_script_vm_modal(s->vm));
    if (!owner || (vk >= 0x70u && vk <= 0x87u)) return 0;
    if (vk == 0x1Bu) {
        if (down) AbortModal(s, "Escape");
        return 1;
    }
    jpt_script_arg args[5]{};
    args[0].type = JPT_SCRIPT_ARG_INT;
    args[0].i = static_cast<int>(vk);
    for (int k = 1; k < 5; ++k) args[k].type = JPT_SCRIPT_ARG_BOOL;
    args[1].b = down != 0;
    args[2].b = shift != 0;
    args[3].b = ctrl != 0;
    args[4].b = alt != 0;
    jpt_script_vm_invoke(s->vm, owner->handle, owner->name.c_str(), "OnModalKey", args, 5);
    if (Alive(s)) ReleaseDeadModal(s);
    return 1;
}

extern "C" int jpt_script_level_modal(const jpt_script_session* s) { return s && s->vm && jpt_script_vm_modal(s->vm) != 0; }

extern "C" unsigned jpt_script_session_user_commands(const jpt_script_session* s) { return s && s->vm ? s->plan.user_commands : 0u; }

extern "C" void jpt_script_level_act_bite(jpt_script_session* s, unsigned animal, unsigned target, const float location[3], float distance,
                                          int feint) {
    if (!s || !s->vm || s->pending || !animal || !location) return;
    for (const Script& script : s->scripts) {
        if (script.kind != Kind::kInstance || script.entity != animal) continue;
        jpt_script_arg args[4]{};
        args[0].type = JPT_SCRIPT_ARG_ENTITY;
        args[0].entity = target;
        args[1].type = JPT_SCRIPT_ARG_VECTOR;
        for (int k = 0; k < 3; ++k) args[1].v[k] = location[k];
        args[2].type = JPT_SCRIPT_ARG_FLOAT;
        args[2].f = distance;
        args[3].type = JPT_SCRIPT_ARG_BOOL;
        args[3].b = feint != 0;
        jpt_script_vm_invoke(s->vm, script.handle, OwnerOf(script.name, script.object).c_str(), "OnActBite", args, 4);
        return;
    }
}

namespace {

const Script* PlayerScript(const jpt_script_session* s, unsigned player) {
    if (!player || !Alive(s) || s->pending) return nullptr;
    for (const Script& script : s->scripts)
        if (script.kind == Kind::kInstance && script.entity == player) return &script;
    return nullptr;
}

// CE keeps the default for a call that fails or returns no number, and 0 for a negative factor.
float Factor(jpt_script_session* s, const Script& script, const char* method, const jpt_script_arg* arg, float fallback) {
    double out = 0.0;
    int has = 0;
    const jpt_script_result result =
        jpt_script_vm_invoke_number(s->vm, script.handle, OwnerOf(script.name, script.object).c_str(), method, arg, arg ? 1 : 0, &out, &has);
    const float f = static_cast<float>(out);
    if (result != JPT_SCRIPT_OK || has != 1 || !std::isfinite(f)) return fallback;
    return f < 0.0f ? 0.0f : f;
}

}  // namespace

extern "C" int jpt_script_level_drives_player(const jpt_script_session* s) {
    if (!s || !Alive(s) || s->pending) return 0;
    return std::any_of(s->scripts.begin(), s->scripts.end(), [](const Script& x) { return x.speed || x.jump_factor; });
}

extern "C" float jpt_script_level_speed_factor(jpt_script_session* s, unsigned player, float speed) {
    const Script* script = s ? PlayerScript(s, player) : nullptr;
    if (!script || !script->speed) return speed;
    jpt_script_arg arg{};
    arg.type = JPT_SCRIPT_ARG_FLOAT;
    arg.f = speed;
    return Factor(s, *script, "fGetSpeedFactor", &arg, speed);
}

extern "C" float jpt_script_level_jump_factor(jpt_script_session* s, unsigned player) {
    const Script* script = s ? PlayerScript(s, player) : nullptr;
    return script && script->jump_factor ? Factor(s, *script, "fGetJumpFactor", nullptr, 1.0f) : 1.0f;
}

extern "C" void jpt_script_level_jump(jpt_script_session* s, unsigned player) {
    const Script* script = s ? PlayerScript(s, player) : nullptr;
    if (script && script->on_jump) jpt_script_vm_invoke(s->vm, script->handle, OwnerOf(script->name, script->object).c_str(), "OnJump", nullptr, 0);
}

extern "C" int jpt_script_level_console(jpt_script_session* s, const char* command, const char* args) {
    if (!s || !command || !*command || !Alive(s) || s->pending) return 0;
    jpt_script_arg a[2]{};
    a[0].type = a[1].type = JPT_SCRIPT_ARG_STRING;
    a[0].s = command;
    a[1].s = args ? args : "";
    for (size_t k = 0; k < s->scripts.size(); ++k) {
        const Script script = s->scripts[k];
        if (script.kind != Kind::kGame || !script.console) continue;
        double out = 0.0;
        int has = 0;
        jpt_script_vm_invoke_number(s->vm, script.handle, script.name.c_str(), "OnConsoleCommand", a, 2, &out, &has);
        if (!Alive(s)) return 0;
        if (has == 2 && out != 0.0) {
            Say(s, "[script] level=%s console command=%s handled by script=%s", s->level.c_str(), command, script.name.c_str());
            return 1;
        }
    }
    return 0;
}

extern "C" uint8_t* jpt_script_session_save(jpt_script_session* s, size_t* size) {
    if (size) *size = 0;
    if (!s || !size) return nullptr;
    Nvram levels, instances;
    if (Alive(s) && !s->pending) {
        All(s, "OnPrepareSave");
        s->game = Capture(s, Kind::kGame);
        levels = Capture(s, Kind::kLevel);
        instances = Capture(s, Kind::kInstance);
    }
    std::vector<Entry> entries;
    const auto add = [&entries](const Nvram& from, Role role, Kind kind) {
        for (const auto& [name, slots] : from) entries.push_back({role, static_cast<uint8_t>(kind), name, "", slots, 0});
    };
    add(s->game, Role::kCurrent, Kind::kGame);
    add(levels, Role::kCurrent, Kind::kLevel);
    for (const Script& script : s->scripts) {
        const auto it = script.kind == Kind::kInstance ? instances.find(OwnerOf(script.name, script.object)) : instances.end();
        if (it == instances.end()) continue;
        const uint8_t flags = jpt_script_vm_stepping(s->vm, script.entity) ? jpt::script::kEntrySimStep : 0;
        entries.push_back({Role::kCurrent, static_cast<uint8_t>(Kind::kInstance), script.name, script.object, it->second, flags});
    }
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
