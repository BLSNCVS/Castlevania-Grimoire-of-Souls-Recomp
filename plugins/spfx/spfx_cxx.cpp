// libc++ and C++ ABI functions the SPFX plugin imports. Its mutexes and
// condition variables are Apple pthread objects inside guest memory; each
// guest address gets a host CRITICAL_SECTION / CONDITION_VARIABLE.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "recomp_bridge.h"
#include "spfx_host.h"

namespace
{
    using recomp::Ctx;

    void* P(uint64_t v) { return reinterpret_cast<void*>(v); }

    std::mutex g_mapLock;

    CRITICAL_SECTION* Section(uint64_t guest)
    {
        std::lock_guard<std::mutex> guard(g_mapLock);
        static std::unordered_map<uint64_t, CRITICAL_SECTION*> sections;
        auto& cs = sections[guest];
        if (!cs)
        {
            cs = new CRITICAL_SECTION;
            InitializeCriticalSection(cs);
        }
        return cs;
    }

    CONDITION_VARIABLE* Condition(uint64_t guest)
    {
        std::lock_guard<std::mutex> guard(g_mapLock);
        static std::unordered_map<uint64_t, CONDITION_VARIABLE*> conditions;
        auto& cv = conditions[guest];
        if (!cv)
        {
            cv = new CONDITION_VARIABLE;
            InitializeConditionVariable(cv);
        }
        return cv;
    }

    // Objects at a guest address may be destroyed and another built there;
    // the host objects are kept (they are stateless once unlocked).
    void Lock(Ctx& c) { EnterCriticalSection(Section(c.x[0])); }
    void Unlock(Ctx& c) { LeaveCriticalSection(Section(c.x[0])); }
    void Nop(Ctx&) {}

    void NotifyAll(Ctx& c) { WakeAllConditionVariable(Condition(c.x[0])); }

    // condition_variable::wait(unique_lock<mutex>&): unique_lock is { mutex*, bool }
    void Wait(Ctx& c)
    {
        uint64_t mutex = *static_cast<uint64_t*>(P(c.x[1]));
        SleepConditionVariableCS(Condition(c.x[0]), Section(mutex), INFINITE);
    }

    void OperatorNew(Ctx& c) { c.x[0] = reinterpret_cast<uint64_t>(_aligned_malloc(c.x[0] ? c.x[0] : 1, 16)); }
    void OperatorDelete(Ctx& c) { _aligned_free(P(c.x[0])); }

    // std::thread is { pthread_t }, our pthread_t a thread HANDLE
    void Join(Ctx& c)
    {
        auto* t = static_cast<uint64_t*>(P(c.x[0]));
        HANDLE h = reinterpret_cast<HANDLE>(*t);
        if (h)
        {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }
        *t = 0;
    }

    void ThreadStructCtor(Ctx& c) { *static_cast<uint64_t*>(P(c.x[0])) = 0; }

    // __thread_specific_ptr<__thread_struct>: { pthread_key_t }
    uint64_t g_threadLocalData = 1;
    void ThreadLocalData(Ctx& c) { c.x[0] = reinterpret_cast<uint64_t>(&g_threadLocalData); }
    thread_local uint64_t t_specific;
    void PthreadSetspecific(Ctx& c)
    {
        t_specific = c.x[1];
        c.x[0] = 0;
        // std::thread's proxy sets this first thing in a new thread
        spfx::NoteThreadStarted();
    }

    // The plugin polls its loader threads with 1 ms sleeps; Sleep(1) lasts a
    // whole 15.6 ms timer tick on Windows, which made startup take a minute.
    // A high-resolution waitable timer sleeps for the time asked.
    void SleepFor(Ctx& c)
    {
        int64_t ns = *static_cast<int64_t*>(P(c.x[0]));
        if (ns <= 0)
        {
            SwitchToThread();
            return;
        }
        // SPFXEngine::Threading::Create polls every 100 ms for its new thread
        // to start: wake as soon as it has
        if (spfx::WaitForThreadStart(DWORD(ns / 1000000)))
            return;
        constexpr DWORD kHighResolution = 0x2;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803)
        thread_local HANDLE timer = [] {
            HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, kHighResolution, TIMER_ALL_ACCESS);
            return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }();
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1, ns / 100);  // relative, 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(DWORD((ns + 999999) / 1000000));
    }

    void SteadyNow(Ctx& c)
    {
        static LARGE_INTEGER freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f;
        }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        c.x[0] = uint64_t(int64_t(double(now.QuadPart) * 1e9 / double(freq.QuadPart)));
    }

    // function-local statics: the guard's first byte is set once initialized
    CRITICAL_SECTION g_guardLock;
    void GuardAcquire(Ctx& c)
    {
        auto* guard = static_cast<uint8_t*>(P(c.x[0]));
        EnterCriticalSection(&g_guardLock);
        if (*guard)
        {
            LeaveCriticalSection(&g_guardLock);
            c.x[0] = 0;
            return;
        }
        c.x[0] = 1;  // the caller initializes, then calls __cxa_guard_release
    }
    void GuardRelease(Ctx& c)
    {
        *static_cast<uint8_t*>(P(c.x[0])) = 1;
        LeaveCriticalSection(&g_guardLock);
    }
    void Atexit(Ctx& c) { c.x[0] = 0; }

    [[noreturn]] void Fatal(Ctx& c)
    {
        spfx::Log("plugin raised a C++ exception or terminated (lr 0x%llx)",
                  static_cast<unsigned long long>(c.x[30] - recomp::g_imageBase));
        abort();
    }

    // Objective-C (NSBundle/NSProcessInfo lookups and the Metal backend, which
    // is never selected): nil for every message
    void MsgSend(Ctx& c)
    {
        static int logged = 0;
        if (logged++ < 20)
            spfx::Log("objc_msgSend %s", c.x[1] ? static_cast<const char*>(P(c.x[1])) : "?");
        c.x[0] = 0;
    }
    void Identity(Ctx&) {}
    void StoreStrong(Ctx& c) { *static_cast<uint64_t*>(P(c.x[0])) = c.x[1]; }
}

namespace spfx
{
    void RegisterCxxImports()
    {
        InitializeCriticalSection(&g_guardLock);
        RegisterHandler("__ZNSt3__15mutex4lockEv", Lock);
        RegisterHandler("__ZNSt3__15mutex6unlockEv", Unlock);
        RegisterHandler("__ZNSt3__15mutexD1Ev", Nop);
        RegisterHandler("__ZNSt3__115recursive_mutex4lockEv", Lock);
        RegisterHandler("__ZNSt3__115recursive_mutex6unlockEv", Unlock);
        RegisterHandler("__ZNSt3__115recursive_mutexC1Ev", Nop);
        RegisterHandler("__ZNSt3__115recursive_mutexD1Ev", Nop);
        RegisterHandler("__ZNSt3__118condition_variable10notify_allEv", NotifyAll);
        RegisterHandler("__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE", Wait);
        RegisterHandler("__ZNSt3__118condition_variableD1Ev", Nop);
        RegisterHandler("__Znwm", OperatorNew);
        RegisterHandler("__Znam", OperatorNew);
        RegisterHandler("__ZdlPv", OperatorDelete);
        RegisterHandler("__ZdaPv", OperatorDelete);
        RegisterHandler("__ZNSt3__16thread4joinEv", Join);
        RegisterHandler("__ZNSt3__16threadD1Ev", Nop);
        RegisterHandler("__ZNSt3__115__thread_structC1Ev", ThreadStructCtor);
        RegisterHandler("__ZNSt3__115__thread_structD1Ev", Nop);
        RegisterHandler("__ZNSt3__119__thread_local_dataEv", ThreadLocalData);
        RegisterHandler("_pthread_setspecific", PthreadSetspecific);
        RegisterHandler("__ZNSt3__111this_thread9sleep_forERKNS_6chrono8durationIxNS_5ratioILl1ELl1000000000EEEEE", SleepFor);
        RegisterHandler("__ZNSt3__16chrono12steady_clock3nowEv", SteadyNow);
        RegisterHandler("___cxa_guard_acquire", GuardAcquire);
        RegisterHandler("___cxa_guard_release", GuardRelease);
        RegisterHandler("___cxa_atexit", Atexit);
        for (const char* s : { "___cxa_begin_catch", "___cxa_call_unexpected", "__Unwind_Resume", "__ZSt9terminatev",
                               "__ZNSt3__120__throw_system_errorEiPKc", "__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv",
                               "__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv",
                               "__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", "___cxa_pure_virtual" })
            RegisterHandler(s, Fatal);
        RegisterHandler("_objc_msgSend", MsgSend);
        for (const char* s : { "_objc_retain", "_objc_release", "_objc_retainAutorelease", "_objc_retainAutoreleasedReturnValue" })
            RegisterHandler(s, Identity);
        RegisterHandler("_objc_storeStrong", StoreStrong);
    }
}
