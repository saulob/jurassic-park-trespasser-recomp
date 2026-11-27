#include "jpt_script_heap.h"

#include <squirrel.h>

#include <stdlib.h>
#include <string.h>

#include <cstddef>

struct alignas(std::max_align_t) ScriptHeap::Block {
    Block* previous;
    Block* next;
    ScriptHeap* heap;
    size_t size;
};

namespace {

thread_local ScriptHeap* g_current = nullptr;

void* SystemAllocate(size_t size) { return malloc(size); }
void* SystemReallocate(void* block, size_t size) { return realloc(block, size); }
void SystemRelease(void* block) { free(block); }

const jpt_script_allocator kSystem = {SystemAllocate, SystemReallocate, SystemRelease};

bool Charge(size_t size, size_t* charge) {
    if (size > SIZE_MAX - ScriptHeap::Overhead()) return false;
    *charge = size + ScriptHeap::Overhead();
    return true;
}

void Add(uint64_t* counter, uint64_t amount) { *counter = amount > UINT64_MAX - *counter ? UINT64_MAX : *counter + amount; }

}  // namespace

ScriptHeap::ScriptHeap(size_t limit, const jpt_script_allocator* system) : system_(system ? *system : kSystem) {
    stats_.limit = limit;
}

ScriptHeap::~ScriptHeap() { ReleaseAll(); }

size_t ScriptHeap::Overhead() { return sizeof(Block); }

ScriptHeap* ScriptHeap::Current() { return g_current; }

void ScriptHeap::Link(Block* block) {
    block->previous = nullptr;
    block->next = blocks_;
    if (blocks_) blocks_->previous = block;
    blocks_ = block;
}

void ScriptHeap::Unlink(Block* block) {
    if (block->previous) block->previous->next = block->next;
    else blocks_ = block->next;
    if (block->next) block->next->previous = block->previous;
}

void ScriptHeap::Fail(bool budget, size_t requested) {
    Add(budget ? &stats_.budget_failures : &stats_.system_failures, 1);
    throw ScriptHeapFailure{budget, requested};
}

void* ScriptHeap::Allocate(size_t size) {
    size_t charge;
    if (!Charge(size, &charge)) Fail(true, SIZE_MAX);
    if (charge > stats_.limit - stats_.current) Fail(true, size);
    Block* block = static_cast<Block*>(system_.allocate(charge));
    if (!block) Fail(false, size);
    block->heap = this;
    block->size = size;
    Link(block);
    stats_.current += charge;
    if (stats_.current > stats_.peak) stats_.peak = stats_.current;
    Add(&stats_.allocations, 1);
    // Squirrel leaves an instance's user data as allocated: an entity a script makes must hold no handle.
    memset(block + 1, 0, size);
    return block + 1;
}

void* ScriptHeap::Reallocate(void* pointer, size_t size) {
    if (!pointer) return Allocate(size);
    Block* block = static_cast<Block*>(pointer) - 1;
    size_t charge;
    if (!Charge(size, &charge)) Fail(true, SIZE_MAX);
    const size_t old_charge = block->size + Overhead();
    if (charge > old_charge && charge - old_charge > stats_.limit - stats_.current) Fail(true, size);
    Unlink(block);
    Block* moved = static_cast<Block*>(system_.reallocate(block, charge));
    if (!moved) {
        Link(block);
        if (charge <= old_charge) return pointer;
        Fail(false, size);
    }
    moved->size = size;
    Link(moved);
    stats_.current = stats_.current - old_charge + charge;
    if (stats_.current > stats_.peak) stats_.peak = stats_.current;
    if (charge < old_charge) Add(&stats_.freed_bytes, old_charge - charge);
    Add(&stats_.reallocations, 1);
    return moved + 1;
}

void ScriptHeap::Release(Block* block) {
    const size_t charge = block->size + Overhead();
    Unlink(block);
    stats_.current -= charge;
    Add(&stats_.frees, 1);
    Add(&stats_.freed_bytes, charge);
    system_.release(block);
}

void ScriptHeap::ReleaseAll() {
    while (blocks_) Release(blocks_);
}

void ScriptHeap::Free(void* pointer) {
    if (!pointer) return;
    Block* block = static_cast<Block*>(pointer) - 1;
    if (block->heap) block->heap->Release(block);
    else SystemRelease(block);
}

ScriptHeapScope::ScriptHeapScope(ScriptHeap* heap) : previous_(g_current) { g_current = heap; }

ScriptHeapScope::~ScriptHeapScope() { g_current = previous_; }

// Squirrel's allocation entry points (squtils.h); sqmem.cpp is built without its own.
void* sq_vm_malloc(SQUnsignedInteger size) {
    if (ScriptHeap* heap = ScriptHeap::Current()) return heap->Allocate(size);
    ScriptHeap::Block* block = nullptr;
    size_t charge;
    if (Charge(size, &charge)) block = static_cast<ScriptHeap::Block*>(malloc(charge));
    if (!block) return nullptr;
    block->heap = nullptr;
    block->size = size;
    return block + 1;
}

void* sq_vm_realloc(void* pointer, SQUnsignedInteger, SQUnsignedInteger size) {
    if (!pointer) return sq_vm_malloc(size);
    ScriptHeap::Block* block = static_cast<ScriptHeap::Block*>(pointer) - 1;
    if (block->heap) return block->heap->Reallocate(pointer, size);
    size_t charge;
    if (!Charge(size, &charge)) return nullptr;
    ScriptHeap::Block* moved = static_cast<ScriptHeap::Block*>(realloc(block, charge));
    if (!moved) return nullptr;
    moved->size = size;
    return moved + 1;
}

void sq_vm_free(void* pointer, SQUnsignedInteger) { ScriptHeap::Free(pointer); }
