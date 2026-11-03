#include "recomp_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace recomp
{
    struct BridgePart
    {
        const BridgeEntry* entries;
        size_t count;
    };

    // Emitted by gen_bridges.py (bridges_table.cpp).
    extern const BridgePart kBridgeParts[];
    extern const size_t kBridgePartCount;

    // Makes the GC scan a guest stack (runtime_imports.cpp); guest frames
    // hold managed references the collector would otherwise not see.
    void RegisterGuestStackRoot(void* base, size_t size);

    namespace
    {
        constexpr size_t kGuestStackSize = 16u << 20;

        struct GuestStack
        {
            uint8_t* base = nullptr;
            uint64_t sp = 0;  // current top for the next entry
        };

        thread_local GuestStack t_stack;

        GuestStack& Stack()
        {
            if (!t_stack.base)
            {
#ifdef _WIN32
                t_stack.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestStackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
                t_stack.base = static_cast<uint8_t*>(mmap(nullptr, kGuestStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#endif
                if (!t_stack.base)
                {
                    fprintf(stderr, "recomp: cannot allocate a guest stack\n");
                    abort();
                }
                RegisterGuestStackRoot(t_stack.base, kGuestStackSize);
                // 16-byte aligned, with headroom above the first frame
                t_stack.sp = (reinterpret_cast<uint64_t>(t_stack.base) + kGuestStackSize - 256) & ~uint64_t(15);
            }
            return t_stack;
        }

        std::unordered_map<uint64_t, Function>& ThunkMap()
        {
            static std::unordered_map<uint64_t, Function> map;
            return map;
        }
    }

    GuestStackScope::GuestStackScope()
    {
        saved = Stack().sp;
    }

    GuestStackScope::~GuestStackScope()
    {
        t_stack.sp = saved;
    }

    uint64_t GuestStackScope::Top() const
    {
        // Leave room below the caller's red zone (arm64 has none, but keep
        // frames apart for debugging) and keep 16-byte alignment.
        return (saved - 64) & ~uint64_t(15);
    }

    void SetGuestStackPointer(uint64_t sp)
    {
        Stack().sp = sp & ~uint64_t(15);
    }

    void InstallBridges()
    {
        auto& map = ThunkMap();
        size_t total = 0;
        for (size_t p = 0; p < kBridgePartCount; p++)
            total += kBridgeParts[p].count;
        map.reserve(total);

        for (size_t p = 0; p < kBridgePartCount; p++)
        {
            for (size_t i = 0; i < kBridgeParts[p].count; i++)
            {
                const BridgeEntry& e = kBridgeParts[p].entries[i];
                uint64_t thunk = reinterpret_cast<uint64_t>(e.thunk);
                memcpy(reinterpret_cast<void*>(g_imageBase + e.slot), &thunk, sizeof(thunk));
                map.emplace(thunk, e.function);
            }
        }
    }

    Function FunctionForThunk(uint64_t hostAddress)
    {
        auto& map = ThunkMap();
        auto it = map.find(hostAddress);
        return it != map.end() ? it->second : nullptr;
    }
}
