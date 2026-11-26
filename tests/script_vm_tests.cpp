// The interpreter of Trespasser CE scripts (jpt_script_vm.cpp), headless: loading, calls, errors, the instruction budget,
// the libraries a script gets and does not get, print() and its cap, the [script] lines.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "jpt_script_vm.h"

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

static void Capture(void*, const char* line) { g_lines.push_back(line); }

static bool Has(const std::string& line) {
    for (const std::string& l : g_lines)
        if (l == line) return true;
    return false;
}

static bool Contains(const std::string& part) {
    for (const std::string& l : g_lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

static int Count(const std::string& prefix) {
    int n = 0;
    for (const std::string& l : g_lines)
        if (!l.compare(0, prefix.size(), prefix)) n++;
    return n;
}

static jpt_script_vm* Vm(long instructions = 0, int print_lines = 0) {
    g_lines.clear();
    jpt_script_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.instructions = instructions;
    config.print_lines = print_lines;
    return jpt_script_vm_create(&config);
}

static jpt_script_result Load(jpt_script_vm* vm, const char* name, const std::string& text) {
    return jpt_script_vm_load(vm, name, text.data(), text.size());
}

static void test_load_and_call() {
    jpt_script_vm* vm = Vm();
    CHECK(vm != nullptr);
    CHECK(Load(vm, "hello.nut", "function Hello() { print(\"hello \" + (2 + 3)); }") == JPT_SCRIPT_OK);
    CHECK(g_lines.empty());
    CHECK(jpt_script_vm_call(vm, "Hello", "Hello") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: hello 5"));
    CHECK(jpt_script_vm_call(vm, "Hello", "Missing") == JPT_SCRIPT_ABSENT);
    CHECK(Load(vm, "value.nut", "NotAFunction <- 3;") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "x", "NotAFunction") == JPT_SCRIPT_ABSENT);
    jpt_script_vm_destroy(vm);
}

static void test_classes_and_state() {
    jpt_script_vm* vm = Vm();
    const char* text =
        "class Counter {\n"
        "  nvram = null;\n"
        "  constructor() { nvram = {}; nvram.n <- 0; }\n"
        "  function Step() { nvram.n++; return nvram.n; }\n"
        "}\n"
        "gCounter <- Counter();\n"
        "function Tick() { print(\"n=\" + gCounter.Step()); }\n";
    CHECK(Load(vm, "counter.nut", text) == JPT_SCRIPT_OK);
    for (int i = 0; i < 1000; i++) CHECK(jpt_script_vm_call(vm, "Counter", "Tick") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: n=1"));
    CHECK(Has("[script] print: n=200"));
    CHECK(!Has("[script] print: n=201"));
    CHECK(Has("[script] print: further output of this level is not logged"));
    jpt_script_vm_destroy(vm);
}

static void test_compile_error() {
    jpt_script_vm* vm = Vm();
    CHECK(Load(vm, "broken.nut", "function A() {\n  local x = ;\n}\n") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] broken.nut line 2 column"));
    CHECK(jpt_script_vm_call(vm, "A", "A") == JPT_SCRIPT_ABSENT);
    jpt_script_vm_destroy(vm);
}

static void test_runtime_error_stops_owner_for_the_level() {
    jpt_script_vm* vm = Vm();
    const char* text =
        "function Good() { print(\"good\"); }\n"
        "function Bad() {\n"
        "  local t = {};\n"
        "  return t.missing;\n"
        "}\n"
        "function Thrown() { throw \"boom\"; }\n"
        "function Caught() { try { throw \"inner\"; } catch (e) { print(\"caught \" + e); } }\n";
    CHECK(Load(vm, "errors.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "Spitter", "Bad") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] Spitter: the index 'missing' does not exist (errors.nut line 4, function Bad)"));
    CHECK(Has("[script] Spitter stops until the level ends"));
    CHECK(jpt_script_vm_disabled(vm, "Spitter"));
    g_lines.clear();
    CHECK(jpt_script_vm_call(vm, "Spitter", "Good") == JPT_SCRIPT_DISABLED);
    CHECK(g_lines.empty());
    CHECK(jpt_script_vm_call(vm, "SpitterHelper", "Good") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: good"));
    CHECK(jpt_script_vm_call(vm, "Hud", "Thrown") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] Hud: boom (errors.nut line 6, function Thrown)"));
    CHECK(jpt_script_vm_call(vm, "Map", "Caught") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: caught inner"));
    jpt_script_vm_end_level(vm);
    CHECK(!jpt_script_vm_disabled(vm, "Spitter"));
    CHECK(jpt_script_vm_call(vm, "Spitter", "Good") == JPT_SCRIPT_OK);
    jpt_script_vm_destroy(vm);
}

static void test_top_level_error() {
    jpt_script_vm* vm = Vm();
    CHECK(Load(vm, "top.nut", "function Later() {}\nlocal a = null;\na.b = 1;\n") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] top.nut: "));
    CHECK(Contains("(top.nut line 3, function main)"));
    CHECK(Has("[script] top.nut stops until the level ends"));
    jpt_script_vm_destroy(vm);
}

static void test_budget() {
    jpt_script_vm* vm = Vm(10000);
    const char* text =
        "function Spin() { while (true) {} }\n"
        "function OneLine() { for (;;); }\n"
        "function Catches() { try { while (true) {} } catch (e) { while (true) {} } }\n"
        "function Swallows() { while (true) { try { while (true) {} } catch (e) {} } }\n"
        "function Recurse(n) { return Recurse(n + 1); }\n"
        "function Deep() { return Recurse(0); }\n"
        "function Sorts() { local a = [3, 1, 2]; a.sort(function(x, y) { while (true) {} }); }\n"
        "function Small() { local s = 0; for (local i = 0; i < 100; i++) s += i; print(\"sum \" + s); }\n";
    CHECK(Load(vm, "budget.nut", text) == JPT_SCRIPT_OK);
    const char* spinners[] = {"Spin", "OneLine", "Catches", "Swallows", "Deep", "Sorts"};
    for (const char* name : spinners) {
        g_lines.clear();
        CHECK(jpt_script_vm_call(vm, name, name) == JPT_SCRIPT_FAILED);
        CHECK(Contains("instruction budget exceeded (10000)") || (!strcmp(name, "Deep") && Count("[script] Deep: ") == 1));
        CHECK(Has(std::string("[script] ") + name + " stops until the level ends"));
    }
    g_lines.clear();
    CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: sum 4950"));
    // The budget is per entry: many small calls never add up to it.
    for (int i = 0; i < 500; i++) CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    jpt_script_vm_destroy(vm);

    vm = Vm(10000);
    CHECK(Load(vm, "loop.nut", "while (true) {}") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] loop.nut: instruction budget exceeded (10000)"));
    jpt_script_vm_destroy(vm);
}

static void test_libraries() {
    jpt_script_vm* vm = Vm();
    const char* text =
        "function Present() {\n"
        "  print(format(\"%d %s\", 3, \"x\"));\n"
        "  print(\"sqrt \" + sqrt(16.0));\n"
        "  local b = blob(4); print(\"blob \" + b.len());\n"
        "  print(\"lower \" + \"ABC\".tolower());\n"
        "  print(\"cos \" + (cos(0.0) == 1.0));\n"
        "  local r = regexp(\"a+\"); print(\"rex \" + r.match(\"aaa\"));\n"
        "}\n"
        "function Absent() {\n"
        "  foreach (n in [\"seterrorhandler\", \"setdebughook\", \"enabledebuginfo\", \"setroottable\", \"setconsttable\",\n"
        "                 \"newthread\", \"suspend\", \"dofile\", \"loadfile\", \"writeclosuretofile\", \"file\", \"system\",\n"
        "                 \"getenv\", \"remove\", \"rename\", \"date\", \"clock\", \"time\"])\n"
        "    print(n + \" \" + (n in getroottable()));\n"
        "}\n";
    CHECK(Load(vm, "libs.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "libs", "Present") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: 3 x"));
    CHECK(Has("[script] print: sqrt 4"));
    CHECK(Has("[script] print: blob 4"));
    CHECK(Has("[script] print: lower abc"));
    CHECK(Has("[script] print: cos true"));
    CHECK(Has("[script] print: rex true"));
    CHECK(jpt_script_vm_call(vm, "libs", "Absent") == JPT_SCRIPT_OK);
    CHECK(Count("[script] print: ") == 6 + 18);
    int present = 0;
    for (size_t i = 6; i < g_lines.size(); i++)
        if (g_lines[i].size() > 5 && !g_lines[i].compare(g_lines[i].size() - 5, 5, " true")) present++;
    CHECK(present == 0);
    jpt_script_vm_destroy(vm);
}

static void test_print_lines_and_log_text() {
    jpt_script_vm* vm = Vm(0, 3);
    const char* text =
        "function Many() { for (local i = 0; i < 5; i++) print(\"line \" + i); }\n"
        "function Split() { print(\"a\\nb\"); }\n"
        "function Forged() { print(\"x\\r[session] fake\\t1\"); }\n"
        "function Raw() { error(\"to stderr\"); }\n";
    CHECK(Load(vm, "print.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "p", "Many") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: line 0") && Has("[script] print: line 2"));
    CHECK(!Has("[script] print: line 3"));
    CHECK(Count("[script] print: further output of this level is not logged") == 1);
    CHECK(jpt_script_vm_call(vm, "p", "Many") == JPT_SCRIPT_OK);
    CHECK(Count("[script] print: further output of this level is not logged") == 1);
    jpt_script_vm_end_level(vm);
    g_lines.clear();
    CHECK(jpt_script_vm_call(vm, "p", "Split") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: a") && Has("[script] print: b"));
    CHECK(jpt_script_vm_call(vm, "p", "Forged") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: x [session] fake 1"));
    CHECK(jpt_script_vm_call(vm, "p", "Raw") == JPT_SCRIPT_OK);
    CHECK(Count("[script] print: ") == 4);
    for (const std::string& l : g_lines) CHECK(!l.compare(0, 9, "[script] ") && l.find_first_of("\r\n\t") == std::string::npos);
    jpt_script_vm_destroy(vm);
}

static void test_files() {
    jpt_script_vm* vm = Vm();
    CHECK(Load(vm, "bom.nut", "\xEF\xBB\xBF" "function Bom() { print(\"bom\"); }") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "bom", "Bom") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: bom"));
    std::string large(JPT_SCRIPT_MAX_SOURCE + 1, ' ');
    CHECK(Load(vm, "large.nut", large) == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] large.nut is larger than"));
    CHECK(Load(vm, "empty.nut", "") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_load(vm, "null.nut", nullptr, 0) == JPT_SCRIPT_FAILED);
    jpt_script_vm_destroy(vm);
    jpt_script_vm_destroy(nullptr);
    CHECK(jpt_script_vm_call(nullptr, "a", "b") == JPT_SCRIPT_ABSENT);
}

int main() {
    test_load_and_call();
    test_classes_and_state();
    test_compile_error();
    test_runtime_error_stops_owner_for_the_level();
    test_top_level_error();
    test_budget();
    test_libraries();
    test_print_lines_and_log_text();
    test_files();
    printf("script_vm_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
