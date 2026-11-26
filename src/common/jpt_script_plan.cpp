#include "jpt_script_plan.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <map>

namespace jpt::script {
namespace {

const char* const kCeNames[] = {
#include "jpt_script_api.inc"
};

const char* const kKeywords[] = {"while", "do", "if", "else", "break", "continue", "return", "null", "function", "local", "for",
                                 "foreach", "in", "typeof", "base", "delete", "try", "catch", "throw", "clone", "yield",
                                 "resume", "switch", "case", "default", "this", "class", "extends", "constructor", "instanceof",
                                 "true", "false", "static", "enum", "const", "__LINE__", "__FILE__", "rawcall"};

// The classes of CE's entity tree that an instance script extends (API-reference.txt).
const char* const kEntities[] = {"CInstance", "CBoundaryBox", "CBackdrop", "CMuzzleFlash", "CHitSpang", "CTerrainObj", "CSocket",
                                 "CEntity", "CAnimate", "CAnimal", "CPlayer", "CEntityAttached", "CCamera", "CEntityLight",
                                 "CTeleport", "CEntityWater", "CWaterDisturbance", "CGun", "CTrigger", "CLocationTrigger",
                                 "CCreatureTrigger", "CObjectTrigger", "CMagnetTrigger", "CStartTrigger", "CCollisionTrigger",
                                 "CSequenceTrigger", "CBooleanTrigger", "CTimerTrigger", "CVariableTrigger", "CMoreMassTrigger",
                                 "CScriptTrigger"};

// ScriptingReadme.txt, by the kind of script that declares them.
const char* const kGameHandlers[] = {"constructor", "destructor", "OnWorldPurge", "OnScriptInit", "OnGameStart", "OnLevelStart",
                                     "OnLevelEnd", "OnLevelTransitionRequested", "OnGameCompleted", "OnPrepareSave", "OnSimStep",
                                     "OnMessage", "OnPaintHW3D", "OnPaint", "OnConsoleCommand", "OnHandleInput", "OnModalAborted",
                                     "OnModalKey", "OnModalChar", "OnModalMouseMove", "OnModalMouseButton", "OnModalMouseWheel",
                                     "OnHandleModalInput"};
const char* const kLevelHandlers[] = {"constructor", "destructor", "OnWorldPurge", "OnScriptInit", "OnLevelStart", "OnLevelEnd",
                                      "OnLevelTransitionRequested", "OnPrepareSave", "OnSimStep", "OnMessage"};
const char* const kActionHandlers[] = {"OnReset", "OnScriptInit", "Run", "OnSimStep", "GetDuration", "OnPrepareSave", "OnPostLoad"};
const char* const kInstanceHandlers[] = {
    "instance_contructor", "OnReset", "OnScriptInit", "OnSimStep", "OnMessage", "OnPrepareSave", "OnPostLoad", "OnSleep",
    "OnAwake", "OnUse", "OnPickUp", "OnDrop", "OnStow", "OnRetrieve", "OnCollision", "OnDamage", "OnDeath", "OnActBite",
    "OnPickedUp", "OnDropped", "OnStowed", "OnRetrieved", "OnDoUse", "fGetJumpFactor", "fGetSpeedFactor", "OnJump",
    "CanFireTrigger", "OnTriggerFired", "OnProcessDeath", "OnProcessMove", "OnProcessDamage", "OnProcessCollision",
    "OnProcessPickUp", "OnProcessUse", "OnProcessTrigger", "OnProcessMoveTriggerTo", "bEvaluateNow"};
// Handlers that only keep a script's own state: a game or level script with no other changes nothing in a level.
const char* const kStateOnly[] = {"constructor", "destructor", "OnScriptInit", "OnPrepareSave", "OnWorldPurge"};

template <size_t N>
bool In(const char* const (&list)[N], const std::string& name) {
    return std::any_of(std::begin(list), std::end(list), [&name](const char* item) { return name == item; });
}

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct Token {
    enum Type { kIdent, kString, kNumber, kPunct } type;
    std::string text;
    int line;
};

bool Lex(std::string_view s, std::vector<Token>& out, std::string& problem) {
    int line = 1;
    size_t i = 0;
    const auto at = [&s](size_t k) { return k < s.size() ? s[k] : '\0'; };
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            ++line;
            ++i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '#' || (c == '/' && at(i + 1) == '/')) {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '/' && at(i + 1) == '*') {
            const size_t end = s.find("*/", i + 2);
            if (end == std::string_view::npos) return problem = "a comment that never ends, line " + std::to_string(line), false;
            line += static_cast<int>(std::count(s.begin() + static_cast<std::ptrdiff_t>(i), s.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
            i = end + 2;
        } else if (c == '"' || c == '\'' || (c == '@' && at(i + 1) == '"')) {
            const bool verbatim = c == '@';
            const char quote = verbatim ? '"' : c;
            const int first = line;
            std::string text;
            i += verbatim ? 2 : 1;
            for (;;) {
                if (i >= s.size()) return problem = "a string that never ends, line " + std::to_string(first), false;
                if (s[i] == '\n') ++line;
                if (verbatim && s[i] == '"' && at(i + 1) == '"') {
                    text += '"';
                    i += 2;
                } else if (s[i] == quote) {
                    ++i;
                    break;
                } else if (!verbatim && s[i] == '\\' && i + 1 < s.size()) {
                    text += s[i + 1];
                    i += 2;
                } else {
                    text += s[i++];
                }
            }
            out.push_back({Token::kString, text, first});
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            const size_t start = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '_')) ++i;
            out.push_back({Token::kNumber, std::string(s.substr(start, i - start)), line});
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t start = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            out.push_back({Token::kIdent, std::string(s.substr(start, i - start)), line});
        } else if ((c == '<' && at(i + 1) == '-') || (c == ':' && at(i + 1) == ':')) {
            out.push_back({Token::kPunct, std::string(s.substr(i, 2)), line});
            i += 2;
        } else {
            out.push_back({Token::kPunct, std::string(1, c), line});
            ++i;
        }
    }
    return true;
}

class Scanner {
public:
    Scanner(FileInfo& file, const std::vector<Token>& tokens) : f_(file), t_(tokens) {}

    void Run() {
        for (size_t i = 0; i < t_.size();) i = Statement(i);
    }

private:
    bool Is(size_t i, const char* text) const { return i < t_.size() && t_[i].text == text && t_[i].type != Token::kString; }
    bool Ident(size_t i) const { return i < t_.size() && t_[i].type == Token::kIdent; }

    void Use(size_t i, std::set<std::string>& into) {
        if (!Ident(i) || In(kKeywords, t_[i].text)) return;
        into.insert(t_[i].text);
    }

    // Locals, parameters, loop and catch variables: never a name of the engine's, whatever they are called.
    void NoteLocals(size_t i) {
        if (Is(i, "local") && Ident(i + 1)) f_.locals.insert(t_[i + 1].text);
        if ((Is(i, "foreach") || Is(i, "catch")) && Is(i + 1, "(")) {
            for (size_t k = i + 2; k < t_.size() && !Is(k, "in") && !Is(k, ")"); ++k)
                if (Ident(k)) f_.locals.insert(t_[k].text);
        }
        if (Is(i, "function")) {
            size_t k = i + 1;
            while (k < t_.size() && !Is(k, "(") && !Is(k, "{")) ++k;
            if (!Is(k, "(")) return;
            int depth = 0;
            bool name = true;
            for (++k; k < t_.size(); ++k) {
                if (Is(k, "(") || Is(k, "[") || Is(k, "{")) ++depth;
                if (Is(k, ")") || Is(k, "]") || Is(k, "}")) {
                    if (depth == 0) break;
                    --depth;
                }
                if (depth == 0 && Is(k, ",")) name = true;
                else if (depth == 0 && name && Ident(k)) {
                    f_.locals.insert(t_[k].text);
                    name = false;
                } else if (depth == 0 && Is(k, "=")) {
                    name = false;
                }
            }
        }
    }

    // From an opening brace at index open to the index after its closing one; every identifier is a use.
    size_t Block(size_t open, std::set<std::string>& uses, ClassInfo* owner) {
        int depth = 0;
        size_t i = open;
        for (; i < t_.size(); ++i) {
            NoteLocals(i);
            if (Is(i, "{")) ++depth;
            if (Is(i, "}") && --depth == 0) return i + 1;
            if (owner && depth == 1) Member(i, *owner);
            Use(i, uses);
        }
        return i;
    }

    void Member(size_t i, ClassInfo& c) {
        if (Is(i, "function") && Ident(i + 1) && Is(i + 2, "(")) c.methods.push_back(t_[i + 1].text);
        if (Is(i, "constructor") && Is(i + 1, "(")) c.methods.push_back("constructor");
        if (!Is(i, "static") || !Is(i + 1, "SCN") || !Is(i + 2, "=")) return;
        if (i + 3 < t_.size() && t_[i + 3].type == Token::kString) {
            c.scenes.push_back(t_[i + 3].text);
            c.has_scenes = true;
            return;
        }
        if (!Is(i + 3, "[")) return;
        std::vector<std::string> scenes;
        size_t k = i + 4;
        for (; k < t_.size() && !Is(k, "]"); ++k) {
            if (t_[k].type == Token::kString) scenes.push_back(t_[k].text);
            else if (!Is(k, ",")) return;
        }
        c.scenes = scenes;
        c.has_scenes = !scenes.empty();
    }

    // A dotted name from i: "A" or "A.B"; next is past it.
    std::string Dotted(size_t i, size_t& next) const {
        std::string name;
        while (Ident(i)) {
            name += t_[i].text;
            if (!Is(i + 1, ".") || !Ident(i + 2)) {
                ++i;
                break;
            }
            name += '.';
            i += 2;
        }
        next = i;
        return name;
    }

    size_t Class(size_t i) {
        ClassInfo c;
        c.line = t_[i].line;
        size_t k = i + 1;
        c.name = Dotted(k, k);
        if (Is(k, "extends")) {
            c.base = Dotted(k + 1, k);
            c.uses.insert(c.base);
        }
        while (k < t_.size() && !Is(k, "{")) ++k;
        if (c.name.find('.') == std::string::npos) f_.globals.insert(c.name);
        const size_t end = Block(k, c.uses, &c);
        f_.classes.push_back(c);
        return end;
    }

    size_t Statement(size_t i) {
        NoteLocals(i);
        if (Is(i, "class") && Ident(i + 1)) return Class(i);
        if (Is(i, "{")) return Block(i, f_.top_uses, nullptr);
        if (Is(i, "function") && Ident(i + 1) && Is(i + 2, "::") && Ident(i + 3)) {
            ClassInfo* owner = nullptr;
            for (ClassInfo& c : f_.classes)
                if (c.name == t_[i + 1].text) owner = &c;
            if (owner) owner->methods.push_back(t_[i + 3].text);
            size_t k = i + 4;
            while (k < t_.size() && !Is(k, "{")) Use(k++, owner ? owner->uses : f_.top_uses);
            NoteLocals(i);
            return Block(k, owner ? owner->uses : f_.top_uses, nullptr);
        }
        if (Is(i, "function") && Ident(i + 1) && Is(i + 2, "(")) f_.globals.insert(t_[i + 1].text);
        if ((Is(i, "enum") || Is(i, "const")) && Ident(i + 1)) f_.globals.insert(t_[i + 1].text);
        if (Ident(i) && Is(i + 1, "<-") && !(i > 0 && Is(i - 1, "."))) f_.globals.insert(t_[i].text);
        Use(i, f_.top_uses);
        return i + 1;
    }

    FileInfo& f_;
    const std::vector<Token>& t_;
};

struct Index {
    explicit Index(const std::vector<FileInfo>& files) : files_(files) {
        for (size_t f = 0; f < files.size(); ++f) {
            for (const ClassInfo& c : files[f].classes) {
                classes[c.name] = {f, &c};
                defined.insert(c.name);
                defined.insert(c.methods.begin(), c.methods.end());
            }
            for (const std::string& g : files[f].globals) {
                declares[g] = f;
                defined.insert(g);
            }
        }
    }

    struct Where {
        size_t file;
        const ClassInfo* info;
    };

    const ClassInfo* Find(const std::string& name) const {
        const auto it = classes.find(name);
        return it == classes.end() ? nullptr : it->second.info;
    }

    // The class and its script bases, nearest first; stops at a base that is not a script's.
    std::vector<const ClassInfo*> Chain(const std::string& name) const {
        std::vector<const ClassInfo*> chain;
        for (const ClassInfo* c = Find(name); c && chain.size() < 32; c = Find(c->base)) {
            if (std::find(chain.begin(), chain.end(), c) != chain.end()) break;
            chain.push_back(c);
        }
        return chain;
    }

    Kind KindOf(const std::string& name) const {
        const std::vector<const ClassInfo*> chain = Chain(name);
        const std::string root = chain.empty() ? name : chain.back()->base;
        if (root == "GameScript") return Kind::kGame;
        if (root == "LevelScript") return Kind::kLevel;
        if (root == "ActionType") return Kind::kAction;
        return In(kEntities, root) ? Kind::kInstance : Kind::kUnknown;
    }

    std::vector<std::string> Methods(const std::string& name) const {
        std::vector<std::string> methods;
        for (const ClassInfo* c : Chain(name)) methods.insert(methods.end(), c->methods.begin(), c->methods.end());
        return methods;
    }

    std::set<std::string> Uses(const std::string& name) const {
        std::set<std::string> uses;
        for (const ClassInfo* c : Chain(name)) uses.insert(c->uses.begin(), c->uses.end());
        return uses;
    }

    bool Missing(const std::string& name, const FileInfo& file) const {
        return IsCeName(name) && !IsImplemented(name) && !defined.count(name) && !file.locals.count(name);
    }

    std::map<std::string, Where> classes;
    std::map<std::string, size_t> declares;
    std::set<std::string> defined;
    const std::vector<FileInfo>& files_;
};

bool KnownHandler(Kind kind, const std::string& name) {
    switch (kind) {
    case Kind::kGame: return In(kGameHandlers, name);
    case Kind::kLevel: return In(kLevelHandlers, name);
    case Kind::kAction: return In(kActionHandlers, name);
    case Kind::kInstance: return In(kInstanceHandlers, name);
    case Kind::kUnknown: break;
    }
    return false;
}

// What a class asks for that the port does not have: CE names it uses, handlers the port does not call.
void Requirements(const Index& index, const std::string& name, Kind kind, std::set<std::string>& out) {
    const auto where = index.classes.find(name);
    if (where == index.classes.end()) return;
    const FileInfo& file = index.files_[where->second.file];
    for (const std::string& use : index.Uses(name))
        if (index.Missing(use, file)) out.insert(use);
    for (const std::string& method : index.Methods(name))
        if (KnownHandler(kind, method) && !IsDrivenHandler(kind, method)) out.insert(method);
}

bool Active(const Index& index, const std::string& name, Kind kind) {
    std::set<std::string> asks;
    Requirements(index, name, kind, asks);
    if (!asks.empty()) return true;
    const std::vector<std::string> methods = index.Methods(name);
    return std::any_of(methods.begin(), methods.end(), [kind](const std::string& m) { return KnownHandler(kind, m) && !In(kStateOnly, m); });
}

bool Applies(const ClassInfo& c, const std::string& scene) {
    if (!c.has_scenes) return true;
    const std::string want = Lower(BaseName(scene));
    return std::any_of(c.scenes.begin(), c.scenes.end(), [&want](const std::string& s) { return Lower(BaseName(s)) == want; });
}

} // namespace

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::kGame: return "game";
    case Kind::kLevel: return "level";
    case Kind::kAction: return "action";
    case Kind::kInstance: return "instance";
    case Kind::kUnknown: break;
    }
    return "unknown";
}

bool IsCeName(const std::string& name) {
    return std::binary_search(std::begin(kCeNames), std::end(kCeNames), name.c_str(),
                              [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
}

const std::vector<ApiEntry>& Api() {
    constexpr unsigned G = kScopeGame, L = kScopeLevel, I = kScopeInstance;
    static const std::vector<ApiEntry> entries = {
#define JPT_API(owner, name, kind, capability) {owner, name, ApiKind::kind, 0, capability},
#define JPT_HANDLER(scopes, name, capability) {"", name, ApiKind::kHandler, scopes, capability},
#include "jpt_script_registry.inc"
#undef JPT_API
#undef JPT_HANDLER
    };
    return entries;
}

bool IsImplemented(const std::string& name) {
    const std::vector<ApiEntry>& api = Api();
    return std::any_of(api.begin(), api.end(), [&name](const ApiEntry& e) { return e.kind != ApiKind::kHandler && name == e.name; });
}

bool IsDrivenHandler(Kind kind, const std::string& name) {
    const unsigned scope = kind == Kind::kGame ? kScopeGame : kind == Kind::kLevel ? kScopeLevel : kind == Kind::kInstance ? kScopeInstance : 0;
    const std::vector<ApiEntry>& api = Api();
    return scope && std::any_of(api.begin(), api.end(), [&](const ApiEntry& e) { return e.kind == ApiKind::kHandler && (e.scopes & scope) && name == e.name; });
}

FileInfo ScanFile(const std::string& path, std::string_view source) {
    FileInfo file;
    file.path = path;
    if (source.size() >= 3 && !std::memcmp(source.data(), "\xEF\xBB\xBF", 3)) source.remove_prefix(3);
    std::vector<Token> tokens;
    if (!Lex(source, tokens, file.problem)) return file;
    Scanner(file, tokens).Run();
    return file;
}

LevelPlan PlanLevel(const std::vector<FileInfo>& files, const std::string& scene, const std::vector<WorldScript>& world) {
    const Index index(files);
    LevelPlan plan;
    std::set<size_t> needed;
    const auto add = [&](const std::string& name, Kind kind, std::vector<std::string> objects) {
        const auto where = index.classes.find(name);
        PlannedClass p;
        p.name = name;
        p.kind = kind;
        p.file = files[where->second.file].path;
        p.objects = std::move(objects);
        const std::vector<std::string> methods = index.Methods(name);
        p.sim_step = std::find(methods.begin(), methods.end(), "OnSimStep") != methods.end();
        plan.classes.push_back(p);
        for (const ClassInfo* c : index.Chain(name)) needed.insert(index.classes.at(c->name).file);
    };
    // A level needs a VM when one of its game or level scripts acts, or its world names a class; then CE makes every one of them.
    const auto applies = [&](const ClassInfo& c, Kind kind) {
        return index.Find(c.name) == &c && index.KindOf(c.name) == kind && (kind == Kind::kGame || Applies(c, scene));
    };
    bool acts = !world.empty();
    for (const Kind kind : {Kind::kGame, Kind::kLevel})
        for (const FileInfo& file : files)
            for (const ClassInfo& c : file.classes)
                acts = acts || (applies(c, kind) && Active(index, c.name, kind));
    for (const Kind kind : {Kind::kGame, Kind::kLevel})
        for (const FileInfo& file : files)
            for (const ClassInfo& c : file.classes)
                if (acts && applies(c, kind)) add(c.name, kind, {});

    std::vector<std::pair<std::string, std::vector<std::string>>> named;
    for (const WorldScript& ref : world) {
        const std::string key = (ref.action ? "1" : "0") + ref.class_name;
        auto it = std::find_if(named.begin(), named.end(), [&key](const auto& n) { return n.first == key; });
        if (it == named.end()) {
            named.push_back({key, {}});
            it = named.end() - 1;
        }
        if (!ref.object.empty()) it->second.push_back(ref.object);
    }
    for (const auto& [key, objects] : named) {
        const bool action = key[0] == '1';
        const std::string name = key.substr(1);
        const Kind kind = index.KindOf(name);
        const std::string who = objects.empty() ? std::string("an object") : objects.front();
        if (!index.Find(name))
            plan.problems.push_back((action ? "an action 99 of " : "") + who + " names the script class " + name + ", which no script of the package declares");
        else if (kind != (action ? Kind::kAction : Kind::kInstance))
            plan.problems.push_back(who + " names the script class " + name + ", " + (kind == Kind::kUnknown ? "a class of no CE kind" : (kind == Kind::kGame || kind == Kind::kLevel ? "a " : "an ") + std::string(KindName(kind)) + " script") +
                                    ", as its " + (action ? "ScriptAT" : "Script"));
        else
            add(name, kind, objects);
    }

    // The files whose globals the needed code reads, until none is added.
    for (bool grew = true; grew;) {
        grew = false;
        std::set<std::string> uses;
        for (const PlannedClass& p : plan.classes) {
            const std::set<std::string> u = index.Uses(p.name);
            uses.insert(u.begin(), u.end());
        }
        for (const size_t f : needed) {
            uses.insert(files[f].top_uses.begin(), files[f].top_uses.end());
            for (const ClassInfo& c : files[f].classes)
                if (index.KindOf(c.name) == Kind::kUnknown) uses.insert(c.uses.begin(), c.uses.end());
        }
        for (const std::string& use : uses) {
            const auto it = index.declares.find(use);
            if (it != index.declares.end() && needed.insert(it->second).second) grew = true;
        }
    }
    for (size_t f = 0; f < files.size(); ++f)
        if (needed.count(f)) plan.files.push_back(files[f].path);

    std::set<std::string> required;
    for (const PlannedClass& p : plan.classes) Requirements(index, p.name, p.kind, required);
    for (const size_t f : needed) {
        for (const std::string& use : files[f].top_uses)
            if (index.Missing(use, files[f])) required.insert(use);
        for (const ClassInfo& c : files[f].classes)
            if (index.Find(c.name) == &c && index.KindOf(c.name) == Kind::kUnknown) Requirements(index, c.name, Kind::kUnknown, required);
    }
    for (const size_t f : needed)
        if (!files[f].problem.empty()) plan.problems.push_back(files[f].path + ": " + files[f].problem);
    plan.required.assign(required.begin(), required.end());
    for (const size_t f : needed) {
        std::set<std::string> uses = files[f].top_uses;
        for (const ClassInfo& c : files[f].classes) uses.insert(c.uses.begin(), c.uses.end());
        for (unsigned n = 0; n < 8; ++n)
            if (uses.count("uCMD_USER" + std::to_string(n + 1))) plan.user_commands |= 1u << n;
    }
    return plan;
}

std::vector<std::string> FileRequires(const std::vector<FileInfo>& files, const FileInfo& file) {
    const Index index(files);
    std::set<std::string> required;
    for (const ClassInfo& c : file.classes) Requirements(index, c.name, index.KindOf(c.name), required);
    for (const std::string& use : file.top_uses)
        if (index.Missing(use, file)) required.insert(use);
    return {required.begin(), required.end()};
}

} // namespace jpt::script
