#include "recomp_ctx.h"
#include "recomp_bridge.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace recomp
{
    uint64_t g_imageBase = 0;
    uint64_t g_imageSize = 0;
    bool (*g_typedNativeCall)(Ctx& c, uint64_t target) = nullptr;

    namespace
    {
        std::unordered_map<uint64_t, Function>& ImportHandlers()
        {
            static std::unordered_map<uint64_t, Function> handlers;
            return handlers;
        }
    }

    void RegisterImport(uint64_t guestAddress, Function handler)
    {
        ImportHandlers()[guestAddress] = handler;
    }

    namespace
    {
        Function Lookup(const FunctionEntry* table, size_t count, uint64_t guestAddress)
        {
            auto end = table + count;
            auto it = std::lower_bound(table, end, guestAddress,
                [](const FunctionEntry& e, uint64_t a) { return e.address < a; });
            return (it != end && it->address == guestAddress) ? it->function : nullptr;
        }
    }

    Function FindFunction(uint64_t guestAddress)
    {
        if (Function f = Lookup(kFunctionTable, kFunctionCount, guestAddress))
            return f;
        return Lookup(kExtraFunctionTable, kExtraFunctionCount, guestAddress);
    }

    void CallImport(Ctx& c, uint64_t guestAddress)
    {
        auto it = ImportHandlers().find(guestAddress);
        if (it != ImportHandlers().end())
        {
            it->second(c);
            return;
        }

        if (Function f = FindFunction(guestAddress))
        {
            f(c);
            return;
        }

        fprintf(stderr, "recomp: call to unhandled import 0x%" PRIx64 "\n", guestAddress);
        abort();
    }

    // Implemented next to libil2cpp (it needs its headers): names the class of
    // the object in x0 and what the bad pointer points at.
    void DescribeBadCall(Ctx& c, uint64_t target);

    namespace
    {
        // Integer/pointer arguments only: x0-x7 go to RCX, RDX, R8, R9 and the
        // stack in order, which matches for every signature without floating
        // point parameters. The result comes back in RAX (x0).
        void CallNativeFallback(Ctx& c, uint64_t target)
        {
            if (g_typedNativeCall && g_typedNativeCall(c, target))
                return;
            static std::mutex lock;
            static std::unordered_set<uint64_t> seen;
            {
                std::lock_guard<std::mutex> guard(lock);
                if (seen.insert(target).second && getenv("GOSR_VERBOSE"))
                {
                    char module[MAX_PATH] = "?";
                    uint64_t base = 0;
#ifdef _WIN32
                    HMODULE m = nullptr;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(target), &m))
                    {
                        GetModuleFileNameA(m, module, sizeof module);
                        base = reinterpret_cast<uint64_t>(m);
                    }
#endif
                    fprintf(stderr, "recomp: native call through a method pointer to %p = %s+0x%" PRIx64 " from guest 0x%" PRIx64 " (integer arguments)\n",
                            reinterpret_cast<void*>(target), module, target - base, c.x[30] - g_imageBase);
                    fflush(stderr);
                }
            }
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(target), &mbi, sizeof(mbi)) ||
                !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            {
                DescribeBadCall(c, target);
                abort();
            }
#endif
            SetGuestStackPointer(c.sp);
            using Fn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            c.x[0] = reinterpret_cast<Fn>(target)(c.x[0], c.x[1], c.x[2], c.x[3], c.x[4], c.x[5], c.x[6], c.x[7]);
        }
    }

    void CallIndirect(Ctx& c, uint64_t hostAddress)
    {
        // Pointers read from patched tables are entry thunks: call the
        // recompiled function directly instead of bouncing through native code.
        if (Function f = FunctionForThunk(hostAddress))
        {
            f(c);
            return;
        }

        uint64_t guest = hostAddress - g_imageBase;
        if (Function f = FindFunction(guest))
        {
            f(c);
            return;
        }

        // Host code outside the game image: method pointers IL2CPP sets to
        // native functions itself (engine icalls of extern methods without a
        // generated body), reached through invokers and delegates.
        if (g_imageSize && guest >= g_imageSize && !ImportHandlers().count(guest))
        {
            CallNativeFallback(c, hostAddress);
            return;
        }

        CallImport(c, guest);
    }

    void Trap(Ctx&, uint64_t guestAddress, uint32_t code)
    {
        fprintf(stderr, "recomp: trap %u at 0x%" PRIx64 "\n", code, guestAddress);
        abort();
    }

    void Unimplemented(Ctx&, uint64_t guestAddress, const char* instruction)
    {
        fprintf(stderr, "recomp: unimplemented instruction at 0x%" PRIx64 ": %s\n", guestAddress, instruction);
        abort();
    }
}
