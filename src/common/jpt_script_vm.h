/* The Squirrel interpreter that runs the scripts of Trespasser CE levels (docs/CE_SCRIPTS.md), custom sessions only.
 * Scripts get the language and its math, string and blob libraries; nothing reaches files, the system or other threads.
 * Every entry into a script runs under an instruction budget: a script that fails or runs out of it is named in a
 * [script] line and its owner is not called again until the level ends. All scripts of a VM share one memory budget:
 * past it the VM is abandoned and its memory freed, and every later call is refused until the host destroys it. */
#ifndef JPT_SCRIPT_VM_H
#define JPT_SCRIPT_VM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jpt_script_vm jpt_script_vm;
typedef void (*jpt_script_log_fn)(void* user, const char* line);
struct jpt_script_allocator;

typedef struct {
    jpt_script_log_fn log;
    void* user;
    long instructions;   /* per entry into a script; 0 = JPT_SCRIPT_DEFAULT_INSTRUCTIONS */
    int print_lines;     /* lines of print() per level; 0 = JPT_SCRIPT_DEFAULT_PRINT_LINES */
    size_t memory_limit; /* bytes for the whole VM, headers included; 0 = JPT_SCRIPT_DEFAULT_MEMORY */
    const struct jpt_script_allocator* allocator; /* NULL = the C runtime's */
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

/* NULL when the VM cannot even open within its memory limit (logged). */
jpt_script_vm* jpt_script_vm_create(const jpt_script_config* config);
void jpt_script_vm_destroy(jpt_script_vm* vm);

/* Compiles a script file and runs its top level, which declares its classes and functions. */
jpt_script_result jpt_script_vm_load(jpt_script_vm* vm, const char* name, const char* source, size_t size);
/* Calls a global function without arguments on behalf of owner. */
jpt_script_result jpt_script_vm_call(jpt_script_vm* vm, const char* owner, const char* function);
int jpt_script_vm_disabled(const jpt_script_vm* vm, const char* owner);
/* The owners stopped in this level run again, and print() logs again; an abandoned VM stays abandoned. */
void jpt_script_vm_end_level(jpt_script_vm* vm);
int jpt_script_vm_memory(const jpt_script_vm* vm, jpt_script_memory* out);

#ifdef __cplusplus
}
#endif

#endif
