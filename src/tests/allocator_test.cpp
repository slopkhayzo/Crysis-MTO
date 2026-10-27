// Exercises the allocator replacement outside the game: header layout, realloc, and (x64) that every block stays below
// 4 GB even after the initial reservation is used up (forces the refill path), the owner tags: a pointer anywhere
// into a live block finds its start, its size and the code that allocated it, and the pins (frees kept, reallocs).
//
// usage: allocator_test.exe [low memory MB]   (default 256, deliberately small)

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "../optiperf/allocator.h"
#include "../optiperf/log.h"

int wmain(int argc, wchar_t** argv) {
    optiperf::LogInit(L"allocator_test.log");
    const size_t lowMb = argc > 1 ? size_t(_wtoi(argv[1])) : 256;
#ifdef _WIN64
    const bool owners = true;
#else
    const bool owners = false;
#endif
    if (!optiperf::Allocator_TestSetup(lowMb, owners)) {
        printf("setup failed\n");
        return 1;
    }
    if (owners != optiperf::Allocator_TracksOwners()) {
        printf("owner tags: mimalloc's page layout is not the expected one\n");
        return 1;
    }
    std::mt19937 rng(1);
    // the owner tags: every block found from a pointer anywhere into it, with the code that allocated it (here the
    // test's wrappers, in this executable)
    auto* exe = reinterpret_cast<const char*>(GetModuleHandleW(nullptr));
    const size_t exeSize =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew)
            ->OptionalHeader.SizeOfImage;
    uint64_t ownerChecks = 0, ownerFailures = 0;
    auto checkOwner = [&](const char* p, size_t size) {
        if (!owners || size == 0) return;  // (a block of 0 bytes has no inside: its pointer is one past its block)
        const void* site = nullptr;
        const char* start = nullptr;
        size_t usable = 0;
        const size_t at = size ? rng() % size : 0;
        ++ownerChecks;
        const bool found = optiperf::Allocator_OwnerOf(p + at, &site, &start, &usable);
        if (!found || start != p || usable < size || size_t(static_cast<const char*>(site) - exe) >= exeSize) {
            if (++ownerFailures <= 5)
                printf("  owner of %p + %zu (a block of %zu): %s, start %p, usable %zu, site %p\n", static_cast<const void*>(p),
                       at, size, found ? "found" : "not found", static_cast<const void*>(start), usable, site);
        }
    };
    struct Block {
        char* p;
        size_t size;
        uint8_t fill;
    };
    std::vector<Block> live;
    uint64_t failures = 0, above4g = 0, total = 0;
    const DWORD t0 = GetTickCount();
    // grow to ~3x the reservation in mixed sizes, freeing and reallocating along the way
    while (total < (lowMb << 20) * 3) {
        const unsigned r = rng() % 100;
        size_t size = r < 70 ? rng() % 512 : r < 95 ? 512 + rng() % 65536 : 65536 + rng() % (4 << 20);
        size_t allocated = 0;
        auto* p = static_cast<char*>(optiperf::Allocator_TestMalloc(size, &allocated));
        if (!p) {
            ++failures;
            break;
        }
        int header;
        std::memcpy(&header, p - 4, 4);
        if (size_t(header) != size || allocated != size + 4 || (reinterpret_cast<uintptr_t>(p) & 15) != 0) ++failures;
        if (uint64_t(reinterpret_cast<uintptr_t>(p)) >> 32) ++above4g;
        const uint8_t fill = uint8_t(rng());
        std::memset(p, fill, size);
        checkOwner(p, size);
        live.push_back({p, size, fill});
        total += size;
        if (live.size() > 64 && rng() % 4 == 0) {  // free a random block
            const size_t i = rng() % live.size();
            if (optiperf::Allocator_TestFree(live[i].p) != live[i].size + 4) ++failures;
            total -= live[i].size;
            live[i] = live.back();
            live.pop_back();
        }
        if (live.size() > 64 && rng() % 8 == 0) {  // grow or shrink a random block; contents must survive
            Block& b = live[rng() % live.size()];
            const size_t newSize = b.size / 2 + rng() % (b.size + 4096);
            auto* q = static_cast<char*>(optiperf::Allocator_TestRealloc(b.p, newSize, &allocated));
            if (!q) {
                ++failures;
                break;
            }
            for (size_t k = 0; k < (newSize < b.size ? newSize : b.size); k += 61)
                if (uint8_t(q[k]) != b.fill) {
                    ++failures;
                    break;
                }
            std::memset(q, b.fill, newSize);
            checkOwner(q, newSize);
            total += newSize - b.size;
            b.p = q;
            b.size = newSize;
            if (uint64_t(reinterpret_cast<uintptr_t>(q)) >> 32) ++above4g;
        }
    }
    const DWORD ms = GetTickCount() - t0;
    for (const Block& b : live) optiperf::Allocator_TestFree(b.p);
    bool pinsOk = true;
#ifdef _WIN64
    {
        // the pins (allocator.h): a pinned block's free is kept until EndPins and the block marked dying; a realloc of
        // one on the pinning thread calls the join first, and (the pins still on) moves it, keeping the old block; on
        // another thread it moves it without a join
        static int joins = 0;
        optiperf::Allocator_EnablePins([] { ++joins; });
        optiperf::Allocator_BeginPins();
        auto alloc = [](size_t n) { return static_cast<char*>(optiperf::Allocator_TestMalloc(n, nullptr)); };
        char *a = alloc(100), *b = alloc(5000), *c = alloc(64), *d = alloc(64);
        int local = 0;
        std::memset(b, 0x3C, 5000);
        std::memset(c, 0x4D, 64);
        const bool pinned = optiperf::Allocator_Pin(a + 10) && optiperf::Allocator_Pin(b) && optiperf::Allocator_Pin(c) &&
                            !optiperf::Allocator_Pin(&local);
        optiperf::Allocator_TestFree(a);  // (kept: still writable)
        std::memset(a, 0x5A, 100);
        const bool dying = optiperf::Allocator_Dying(a + 50) && !optiperf::Allocator_Dying(b) &&
                           !optiperf::Allocator_Dying(d);
        char* const b2 = static_cast<char*>(optiperf::Allocator_TestRealloc(b, 20000, nullptr));
        const bool movedHere = joins == 1 && b2 && b2 != b && optiperf::Allocator_Dying(b) &&
                               uint8_t(b2[4999]) == 0x3C && !optiperf::Allocator_Dying(b2);
        char* c2 = nullptr;
        std::thread([&] { c2 = static_cast<char*>(optiperf::Allocator_TestRealloc(c, 1000, nullptr)); }).join();
        const bool movedThere = joins == 1 && c2 && c2 != c && optiperf::Allocator_Dying(c) && uint8_t(c2[63]) == 0x4D;
        optiperf::Allocator_TestFree(d);  // (not pinned: freed at once)
        optiperf::Allocator_EndPins();
        const bool ended = !optiperf::Allocator_Dying(b2) && !optiperf::Allocator_Pin(b2);  // (no pins outside a window)
        optiperf::Allocator_TestFree(b2);
        optiperf::Allocator_TestFree(c2);
        pinsOk = pinned && dying && movedHere && movedThere && ended;
        printf("pins: pinned %d, the freed one dying %d, a realloc here joined and moved %d, on another thread moved %d, "
               "ended %d\n",
               pinned, dying, movedHere, movedThere, ended);
    }
#endif
    optiperf::Allocator_Report("test");
    printf("%zu blocks live at the peak, %.0f MB, %lu ms: %llu failures, %llu blocks above 4 GB\n", live.size(),
           total / 1048576.0, ms, failures, above4g);
    printf("owner tags: %llu blocks looked up from inside, %llu wrong\n", ownerChecks, ownerFailures);
    const bool ok = failures == 0 && above4g == 0 && ownerFailures == 0 && (!owners || ownerChecks > 0) && pinsOk;
    printf(ok ? "OK\n" : "FAILED\n");
    optiperf::LogClose();
    return ok ? 0 : 1;
}
