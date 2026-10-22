#include "ring_fix.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game.hpp"
#include "log.hpp"

namespace tc2vr::ring_fix {
namespace {

constexpr uintptr_t kAllocRva = 0x20d33e0;
constexpr uintptr_t kFindPageRva = 0x20d3220;
constexpr uintptr_t kFallbackRva = 0x20d3130;

// First 16 bytes of FUN_1420d33e0, verified before patching.
constexpr uint8_t kPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48,
                                   0x89, 0x6c, 0x24, 0x20, 0x4c, 0x89,
                                   0x44, 0x24, 0x18, 0x56};

// Ring page: cursor, capacity, buffer, mapped pointer, mapped base.
struct Page {
    uint32_t cursor;
    uint32_t capacity;
    void* buffer;
    uint8_t* mapped;
    uint32_t mappedBase;
    uint32_t reserved;
};

constexpr size_t kRingCurrentPage = 0x18;
constexpr size_t kRingAllocCount = 0x20;

using FindPageFn = Page* (*)(void* ring, uint32_t size, uint8_t single,
                             uint8_t exclusive);
using FallbackFn = uint64_t (*)(void* ring, uint32_t size, void** outBuffer,
                                uint32_t* outOffset, uint8_t** outPtr,
                                uint64_t name, uint8_t heap);
using MapFn = bool (*)(void* buffer, uint32_t offset, uint32_t size,
                       uint8_t** mapped, uint32_t mode);

// Same contract as FUN_1420d33e0, minus the protector detour.
uint64_t RingAlloc(uint8_t* ring, uint32_t size, void** outBuffer,
                   uint32_t* outOffset, uint8_t** outPtr, uint64_t name,
                   uint32_t flags) {
    uint32_t aligned = (size + 0x1e) & 0xfffffff0u;
    uint8_t heap = flags & 1;
    *outPtr = nullptr;
    auto findPage = game::At<FindPageFn>(kFindPageRva);
    auto fallback = game::At<FallbackFn>(kFallbackRva);
    Page* page = findPage(ring, aligned, (flags >> 1) & 1, (flags >> 2) & 1);
    if (!page) {
        return fallback(ring, aligned, outBuffer, outOffset, outPtr, name,
                        heap);
    }
    uint32_t cursor = page->cursor;
    if (!page->mapped) {
        auto map = (*reinterpret_cast<MapFn**>(page->buffer))[2];
        uint32_t mode = cursor != 0 ? 2 : 1;
        if (!map(page->buffer, cursor, page->capacity - cursor,
                 &page->mapped, mode)) {
            return fallback(ring, aligned, outBuffer, outOffset, outPtr,
                            name, heap);
        }
        page->mappedBase = cursor;
        page->reserved = 0;
    }
    auto mapped = reinterpret_cast<uintptr_t>(page->mapped);
    uint64_t offset = ((mapped + 0xf) & ~uintptr_t(0xf)) - mapped + cursor;
    *outPtr = page->mapped - page->mappedBase + offset;
    game::Write<Page*>(ring, kRingCurrentPage, page);
    *outOffset = static_cast<uint32_t>(offset);
    *outBuffer = page->buffer;
    page->cursor += aligned;
    uint32_t count = game::Read<uint32_t>(ring, kRingAllocCount) + 1;
    game::Write<uint32_t>(ring, kRingAllocCount, count);
    return count;
}

using NtProtectFn = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);

// The protector may refuse requests made through the current-process
// pseudo-handle, so try a real process handle first, then ntdll.
bool Protect(void* addr, SIZE_T size, ULONG prot, ULONG* old) {
    static HANDLE self = OpenProcess(PROCESS_VM_OPERATION, FALSE,
                                     GetCurrentProcessId());
    static auto ntProtect = reinterpret_cast<NtProtectFn>(GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtProtectVirtualMemory"));
    if (self && VirtualProtectEx(self, addr, size, prot, old)) {
        return true;
    }
    LOG("ring fix: VirtualProtectEx with process handle failed (%lu)",
        GetLastError());
    HANDLE handles[2] = {self, GetCurrentProcess()};
    for (HANDLE handle : handles) {
        if (!handle || !ntProtect) {
            continue;
        }
        PVOID base = addr;
        SIZE_T length = size;
        LONG status = ntProtect(handle, &base, &length, prot, old);
        if (status >= 0) {
            return true;
        }
        LOG("ring fix: NtProtectVirtualMemory (%s handle) status 0x%lx",
            handle == self ? "real" : "pseudo",
            static_cast<unsigned long>(status));
    }
    return false;
}

}  // namespace

bool Install() {
    auto target = game::At<uint8_t*>(kAllocRva);
    if (memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        LOG("ring fix: prologue mismatch, not installed");
        return false;
    }
    // mov rax, imm64; jmp rax; then the original bytes 12..15.
    alignas(16) uint8_t patch[16];
    memcpy(patch, kPrologue, sizeof(patch));
    patch[0] = 0x48;
    patch[1] = 0xb8;
    void* destination = reinterpret_cast<void*>(&RingAlloc);
    memcpy(patch + 2, &destination, sizeof(destination));
    patch[10] = 0xff;
    patch[11] = 0xe0;
    ULONG old = 0;
    if (!Protect(target, 16, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    unsigned __int128 expected;
    unsigned __int128 desired;
    memcpy(&expected, kPrologue, 16);
    memcpy(&desired, patch, 16);
    bool ok = __sync_bool_compare_and_swap(
        reinterpret_cast<unsigned __int128*>(target), expected, desired);
    ULONG ignored = 0;
    Protect(target, 16, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    LOG("ring fix: %s", ok ? "installed" : "write failed");
    return ok;
}

}  // namespace tc2vr::ring_fix
