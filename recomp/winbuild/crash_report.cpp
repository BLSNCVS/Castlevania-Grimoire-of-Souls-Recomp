// Prints a stack trace (module + offset) when the process aborts or crashes,
// so failures in recompiled code can be traced back to a function.
#include <csignal>
#include <cstdio>
#include <cwchar>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

namespace
{
    void PrintFrame(void* pc)
    {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(pc), &module))
        {
            GetModuleFileNameA(module, name, MAX_PATH);
        }
        const char* base = name;
        for (const char* p = name; *p; p++)
        {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }
        fprintf(stderr, "  %p %s+0x%llx\n", pc, base,
                static_cast<unsigned long long>(static_cast<char*>(pc) - reinterpret_cast<char*>(module)));
    }

    void PrintStack(const char* why)
    {
        fprintf(stderr, "GoSRecomp: %s, stack:\n", why);
        void* frames[64];
        USHORT n = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
        for (USHORT i = 0; i < n; i++)
            PrintFrame(frames[i]);
        fflush(stderr);
    }

    void OnAbort(int)
    {
        PrintStack("abort");
    }

    thread_local bool t_probing = false;  // set while the sampler probes engine state

    LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
    {
        if (t_probing)
            return EXCEPTION_CONTINUE_SEARCH;
        DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO)
        {
            fprintf(stderr, "GoSRecomp: exception %08lx at ", code);
            PrintFrame(info->ExceptionRecord->ExceptionAddress);
            if (code == EXCEPTION_ACCESS_VIOLATION)
                fprintf(stderr, "  accessing %p\n", reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
            PrintStack("crash");
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    struct Install
    {
        Install()
        {
            // The runtime's reports go to gosr_log.txt next to the game: a
            // GUI process on Windows has no stderr, and Unity's Player.log
            // only carries the engine's own output. GOSR_LOG=- keeps stderr.
            wchar_t mode[4] = L"";
            GetEnvironmentVariableW(L"GOSR_LOG", mode, 4);
            if (lstrcmpW(mode, L"-") != 0)
            {
                wchar_t path[MAX_PATH];
                DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
                while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/')
                    len--;
                if (len + 13 < MAX_PATH)
                {
                    lstrcpyW(path + len, L"gosr_log.txt");
                    if (_wfreopen(path, L"w", stderr))
                        setvbuf(stderr, nullptr, _IONBF, 0);
                }
            }
            // Crash reports are always on: they name the failing recompiled function.
            if (GetEnvironmentVariableW(L"GOSR_NO_CRASH_TRACE", nullptr, 0))
                return;
            signal(SIGABRT, OnAbort);
            AddVectoredExceptionHandler(0, OnCrash);
        }
    } g_install;
}

namespace recomp
{
    // Extra state printed with each sample (set by runtime_imports.cpp).
    void (*g_sampleStateHook)() = nullptr;
    namespace
    {
        // Modules and their unwind tables, gathered while no thread is
        // suspended: the walk itself must not take the loader lock (or any
        // other) that the suspended thread may hold.
        struct ModuleInfo
        {
            uintptr_t base, end;
            const RUNTIME_FUNCTION* table;
            DWORD count;
            char name[64];
        };

        int SnapshotModules(ModuleInfo* out, int max)
        {
            HMODULE mods[256];
            DWORD needed = 0;
            if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
                return 0;
            int n = 0;
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && n < max; i++)
            {
                auto base = reinterpret_cast<uintptr_t>(mods[i]);
                auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                const IMAGE_DATA_DIRECTORY& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                ModuleInfo& m = out[n++];
                m.base = base;
                m.end = base + nt->OptionalHeader.SizeOfImage;
                m.table = ex.VirtualAddress ? reinterpret_cast<const RUNTIME_FUNCTION*>(base + ex.VirtualAddress) : nullptr;
                m.count = ex.Size / sizeof(RUNTIME_FUNCTION);
                char path[MAX_PATH] = "?";
                GetModuleFileNameA(mods[i], path, MAX_PATH);
                const char* b = path;
                for (const char* p = path; *p; p++)
                    if (*p == '\\' || *p == '/')
                        b = p + 1;
                lstrcpynA(m.name, b, sizeof(m.name));
            }
            return n;
        }

        const ModuleInfo* FindModule(const ModuleInfo* mods, int n, uintptr_t pc)
        {
            for (int i = 0; i < n; i++)
                if (pc >= mods[i].base && pc < mods[i].end)
                    return &mods[i];
            return nullptr;
        }

        PRUNTIME_FUNCTION Lookup(const ModuleInfo* m, uintptr_t pc)
        {
            if (!m || !m->table)
                return nullptr;
            DWORD rva = DWORD(pc - m->base), lo = 0, hi = m->count;
            while (lo < hi)
            {
                DWORD mid = (lo + hi) / 2;
                if (rva < m->table[mid].BeginAddress)
                    hi = mid;
                else if (rva >= m->table[mid].EndAddress)
                    lo = mid + 1;
                else
                    return const_cast<PRUNTIME_FUNCTION>(&m->table[mid]);
            }
            return nullptr;
        }

        // Walks a suspended thread's stack into `pcs` without locks or allocation.
        int WalkThread(HANDLE thread, const ModuleInfo* mods, int nmods, uintptr_t* pcs, int max)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(thread, &ctx))
                return 0;
            int n = 0;
            while (n < max && ctx.Rip)
            {
                pcs[n++] = ctx.Rip;
                const ModuleInfo* m = FindModule(mods, nmods, ctx.Rip);
                if (!m)
                    break;  // unknown code: stop rather than guess
                PRUNTIME_FUNCTION fn = Lookup(m, ctx.Rip);
                if (!fn)
                {
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);  // leaf
                    ctx.Rsp += 8;
                    continue;
                }
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
            return n;
        }

        // The probe reads engine state from another thread; a fault there must
        // not take the game down.
        void CallStateHook()
        {
            t_probing = true;
            __try
            {
                g_sampleStateHook();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            t_probing = false;
        }

        DWORD WINAPI SamplerThread(void* param)
        {
            DWORD mainId = DWORD(reinterpret_cast<uintptr_t>(param));
            HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, mainId);
            if (!main)
                return 0;
            wchar_t value[16] = L"";
            DWORD ms = 3000, count = 0xFFFFFFFF;  // the whole session
            if (GetEnvironmentVariableW(L"GOSR_SAMPLE_SECONDS", value, 16))
                ms = wcstoul(value, nullptr, 10) * 1000;
            if (GetEnvironmentVariableW(L"GOSR_SAMPLE_MS", value, 16))
                ms = wcstoul(value, nullptr, 10), count = 2000;
            // Under Wine, reading a suspended thread's context can block with
            // the thread still suspended, freezing the game: off unless asked
            bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
            if (wine && !GetEnvironmentVariableW(L"GOSR_SAMPLE_SECONDS", value, 16) && !GetEnvironmentVariableW(L"GOSR_SAMPLE_MS", value, 16))
                ms = 0;
            if (ms == 0)
                return 0;
            static ModuleInfo mods[256];
            for (DWORD n = 0; n < count; n++)
            {
                Sleep(ms);
                int nmods = SnapshotModules(mods, 256);
                uintptr_t pcs[48];
                if (SuspendThread(main) == DWORD(-1))
                    break;
                int depth = WalkThread(main, mods, nmods, pcs, 48);
                ResumeThread(main);
                fprintf(stderr, "sample %lu (%lums):\nsample:", n, (n + 1) * ms);
                for (int i = 0; i < depth; i++)
                {
                    const ModuleInfo* m = FindModule(mods, nmods, pcs[i]);
                    fprintf(stderr, " %s+0x%llx", m ? m->name : "?", static_cast<unsigned long long>(m ? pcs[i] - m->base : pcs[i]));
                }
                fputc('\n', stderr);
                if (g_sampleStateHook && n >= 3)
                    CallStateHook();
                fflush(stderr);
            }
            CloseHandle(main);
            return 0;
        }
    }

    // Every few seconds, logs where the main thread is (GOSR_SAMPLE_SECONDS,
    // 0 = off): shows what the game waits on when nothing visibly happens.
    void StartMainThreadSampler()
    {
        HANDLE t = CreateThread(nullptr, 0, SamplerThread, reinterpret_cast<void*>(uintptr_t(GetCurrentThreadId())), 0, nullptr);
        if (t)
            CloseHandle(t);
    }
}
