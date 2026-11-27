/* The Squirrel interpreter that runs the scripts of Trespasser CE levels (docs/CE_SCRIPTS.md), custom sessions only.
 * Scripts get the language and its math, string and blob libraries; nothing reaches files, the system or other threads.
 * Every entry into a script runs under an instruction budget: a script that fails or runs out of it is named in a
 * [script] line (level, script, file, callback, line) and its owner is not called again until the level ends. All scripts
 * of a VM share one memory budget: past it the VM is abandoned and its memory freed, and every later call is refused until
 * the host destroys it. */
#ifndef JPT_SCRIPT_VM_H
#define JPT_SCRIPT_VM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpt_script_vm jpt_script_vm;
typedef void (*jpt_script_log_fn)(void* user, const char* line);
struct jpt_script_allocator;
struct jpt_script_engine;

typedef struct {
    jpt_script_log_fn log;
    void* user;
    long instructions;   /* per entry into a script; 0 = JPT_SCRIPT_DEFAULT_INSTRUCTIONS */
    int print_lines;     /* lines of print() per level; 0 = JPT_SCRIPT_DEFAULT_PRINT_LINES */
    size_t memory_limit; /* bytes for the whole VM, headers included; 0 = JPT_SCRIPT_DEFAULT_MEMORY */
    const struct jpt_script_allocator* allocator; /* NULL = the C runtime's */
    const struct jpt_script_engine* engine;       /* the world CE's API reaches; NULL: none */
    unsigned random_seed;                         /* of TP_Engine's random numbers; 0 = a fixed one */
} jpt_script_config;

#define JPT_SCRIPT_DEFAULT_INSTRUCTIONS 1000000L
#define JPT_SCRIPT_DEFAULT_PRINT_LINES 200
#define JPT_SCRIPT_DEFAULT_MEMORY ((size_t)16 * 1024 * 1024)
#define JPT_SCRIPT_MAX_SOURCE (1024u * 1024u)

typedef enum {
    JPT_SCRIPT_OK,
    JPT_SCRIPT_ABSENT,   /* no such function: not an error, a handler a script does not declare */
    JPT_SCRIPT_FAILED,   /* compile error, runtime error, a budget exhausted; already logged */
    JPT_SCRIPT_DISABLED  /* the owner failed earlier in this level, or the VM ran out of memory */
} jpt_script_result;

typedef struct {
    size_t current;
    size_t peak;
    size_t limit;
    unsigned long long allocations;
    unsigned long long reallocations;
    unsigned long long frees;
    unsigned long long freed_bytes;
    unsigned long long budget_failures;
    unsigned long long system_failures;
    int abandoned;
} jpt_script_memory;

typedef enum {
    JPT_SCRIPT_ARG_BOOL,
    JPT_SCRIPT_ARG_FLOAT,
    JPT_SCRIPT_ARG_STRING,
    JPT_SCRIPT_ARG_INT,
    JPT_SCRIPT_ARG_VECTOR, /* v[0..2] as a Vector3 */
    JPT_SCRIPT_ARG_ENTITY  /* the world's object of unique handle entity, null when 0 */
} jpt_script_arg_type;
typedef struct {
    jpt_script_arg_type type;
    int b;
    float f;
    const char* s;
    int i;
    float v[3];
    unsigned entity;
} jpt_script_arg;

/* NULL when the VM cannot even open within its memory limit (logged). */
jpt_script_vm* jpt_script_vm_create(const jpt_script_config* config);
void jpt_script_vm_destroy(jpt_script_vm* vm);
/* The level its [script] lines name. */
void jpt_script_vm_set_level(jpt_script_vm* vm, const char* level);

/* Compiles a script file and runs its top level, which declares its classes and functions. */
jpt_script_result jpt_script_vm_load(jpt_script_vm* vm, const char* name, const char* source, size_t size);
/* Calls a global function without arguments on behalf of owner. */
jpt_script_result jpt_script_vm_call(jpt_script_vm* vm, const char* owner, const char* function);
int jpt_script_vm_disabled(const jpt_script_vm* vm, const char* owner);
/* The owners stopped in this level run again, and print() logs again; an abandoned VM stays abandoned. */
void jpt_script_vm_end_level(jpt_script_vm* vm);
int jpt_script_vm_memory(const jpt_script_vm* vm, jpt_script_memory* out);

/* An empty class of the root table for scripts to extend. */
int jpt_script_vm_define_class(jpt_script_vm* vm, const char* name);
/* Whether the root table's class name is base or derives from it. */
int jpt_script_vm_extends(jpt_script_vm* vm, const char* name, const char* base);
/* An instance of the root table's class, its constructor run on behalf of owner: a handle above 0, or 0 when it failed
 * (logged, and owner is stopped until the level ends). */
int jpt_script_vm_instantiate(jpt_script_vm* vm, const char* owner, const char* class_name);
int jpt_script_vm_has_method(jpt_script_vm* vm, int instance, const char* method);

/* The world the VM's objects belong to: generation, and whether it is loaded (CE's API reaches the engine only then). */
void jpt_script_vm_set_world(jpt_script_vm* vm, unsigned generation, int ready);
/* The CE entity class (CInstance, CAnimal, ...) the root table's class derives from, nearest first; "" when none. */
const char* jpt_script_vm_native_base(jpt_script_vm* vm, const char* class_name);
/* An instance script: an instance of the class for the world's object of that handle, its constructor run on behalf of owner.
 * A handle above 0, or 0 when it failed (logged, and owner stopped until the level ends). */
int jpt_script_vm_attach(jpt_script_vm* vm, const char* owner, const char* class_name, unsigned entity);
/* A game or level script: SendLGMessage reaches it, in the order given. */
void jpt_script_vm_set_receiver(jpt_script_vm* vm, int instance);
/* ActivateSimStep of an object's instance script. */
int jpt_script_vm_stepping(const jpt_script_vm* vm, unsigned entity);
void jpt_script_vm_set_stepping(jpt_script_vm* vm, unsigned entity, int on);
/* Calls a method and keeps its result as a number (a bool is 0 or 1): JPT_SCRIPT_OK with *has 1 when it returned a number,
 * 2 when a bool. */
jpt_script_result jpt_script_vm_invoke_number(jpt_script_vm* vm, int instance, const char* owner, const char* method, const jpt_script_arg* args,
                                              int count, double* out, int* has);
/* OnPaintHW3D: DrawTexture is allowed in between; the frame's commands start empty. */
void jpt_script_vm_begin_paint(jpt_script_vm* vm);
void jpt_script_vm_end_paint(jpt_script_vm* vm);
/* Leaves out the commands an instance drew (its paint failed). */
void jpt_script_vm_drop_draws(jpt_script_vm* vm, int instance);
/* The input a game script reads in OnHandleInput and OnHandleModalInput. */
void jpt_script_vm_set_input(jpt_script_vm* vm, int reading, uint32_t user_held, uint32_t user_hit, float move_x, float move_y);
/* The game script in modal mode, 0 for none; release it (no OnModalAborted). */
int jpt_script_vm_modal(const jpt_script_vm* vm);
void jpt_script_vm_release_modal(jpt_script_vm* vm);
/* Calls a method of an instance with the instance as this. */
jpt_script_result jpt_script_vm_invoke(jpt_script_vm* vm, int instance, const char* owner, const char* method, const jpt_script_arg* args,
                                       int count);

#ifdef __cplusplus
}

#include <string>
#include <vector>

#include "jpt_script_state.h"

/* The instance's nvram table as values: false when it has none. notes: what of it a save cannot hold. */
bool jpt_script_vm_get_nvram(jpt_script_vm* vm, int instance, std::vector<jpt::script::Slot>& out, std::vector<std::string>& notes);
/* Saved slots put into the instance's nvram table; slots it has that are not saved stay. false when it has no table. */
bool jpt_script_vm_set_nvram(jpt_script_vm* vm, int instance, const std::vector<jpt::script::Slot>& slots);

#include "jpt_script_draw.h"
/* The commands of the last paint, and the VM's textures by id (valid until the next paint or the VM's end). */
const std::vector<jpt_draw_cmd_t>& jpt_script_vm_draw_list(const jpt_script_vm* vm);
bool jpt_script_vm_texture(const jpt_script_vm* vm, uint32_t id, jpt_draw_texture_t* out);
size_t jpt_script_vm_texture_count(const jpt_script_vm* vm);
#endif

#endif
