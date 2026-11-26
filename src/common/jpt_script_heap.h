// The memory of one script VM: every allocation Squirrel makes while a heap is current is charged to it, headers
// included, and refused past its limit by throwing ScriptHeapFailure; the VM is then abandoned and ReleaseAll frees
// what it held without running any of Squirrel's code. Blocks carry their heap, so a free never needs a current one.
#ifndef JPT_SCRIPT_HEAP_H
#define JPT_SCRIPT_HEAP_H

#include <stddef.h>
#include <stdint.h>

struct jpt_script_allocator {
    void* (*allocate)(size_t size);
    void* (*reallocate)(void* block, size_t size);
    void (*release)(void* block);
};

struct ScriptHeapFailure {
    bool budget;      // false: the system refused memory the budget allowed
    size_t requested; // bytes asked for, header excluded; SIZE_MAX when the size overflowed
};

struct ScriptHeapStats {
    size_t current = 0;
    size_t peak = 0;
    size_t limit = 0;
    uint64_t allocations = 0;
    uint64_t reallocations = 0;
    uint64_t frees = 0;
    uint64_t freed_bytes = 0;
    uint64_t budget_failures = 0;
    uint64_t system_failures = 0;
};

class ScriptHeap {
public:
    explicit ScriptHeap(size_t limit, const jpt_script_allocator* system = nullptr);
    ~ScriptHeap();
    ScriptHeap(const ScriptHeap&) = delete;
    ScriptHeap& operator=(const ScriptHeap&) = delete;

    void* Allocate(size_t size);
    void* Reallocate(void* pointer, size_t size);
    void ReleaseAll();
    const ScriptHeapStats& Stats() const { return stats_; }

    static size_t Overhead();
    static ScriptHeap* Current();
    // Frees a block of any heap, or one made while no heap was current.
    static void Free(void* pointer);

    struct Block;

private:
    void Link(Block* block);
    void Unlink(Block* block);
    void Release(Block* block);
    [[noreturn]] void Fail(bool budget, size_t requested);

    jpt_script_allocator system_;
    ScriptHeapStats stats_;
    Block* blocks_ = nullptr;
};

class ScriptHeapScope {
public:
    explicit ScriptHeapScope(ScriptHeap* heap);
    ~ScriptHeapScope();
    ScriptHeapScope(const ScriptHeapScope&) = delete;
    ScriptHeapScope& operator=(const ScriptHeapScope&) = delete;

private:
    ScriptHeap* previous_;
};

#endif
