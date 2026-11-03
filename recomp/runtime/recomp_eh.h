#pragma once

// Exception support for recompiled code. Calls inside protected ranges of the
// original code (its LSDA) are wrapped in native try/catch blocks; when an
// exception arrives, these helpers set up x0/x1 as the original landing pad
// expects (exception object, selector) before jumping to it.

#include "recomp_ctx.h"

struct Il2CppException;

// Must match libil2cpp/il2cpp-object-internals.h: MSVC matches catch clauses
// by type name, including struct vs class.
struct Il2CppExceptionWrapper
{
    Il2CppException* ex;
    Il2CppExceptionWrapper(Il2CppException* ex) : ex(ex) {}
};

namespace recomp
{
    // Callee-saved state of a frame at a protected call. Unwinding restores
    // these before the landing pad runs; recompiled frames skipped by a C++
    // exception never run their epilogues, so the catching frame restores
    // its own values from this snapshot.
    struct CalleeSaved
    {
        uint64_t x[11];  // x19-x29
        uint64_t sp;
        uint64_t d[8];   // d8-d15

        explicit CalleeSaved(const Ctx& c)
        {
            for (int i = 0; i < 11; i++)
                x[i] = c.x[19 + i];
            sp = c.sp;
            for (int i = 0; i < 8; i++)
                d[i] = c.v[8 + i].u64[0];
        }

        void Restore(Ctx& c) const
        {
            for (int i = 0; i < 11; i++)
                c.x[19 + i] = x[i];
            c.sp = sp;
            for (int i = 0; i < 8; i++)
                c.v[8 + i].u64[0] = d[i];
        }
    };

    // catch clause for IL2CPP exceptions: x0 -> caught object, x1 = selector
    void LandingCatch(Ctx& c, const Il2CppExceptionWrapper& e, int selector);
    // catch(...) or a foreign exception caught by a catch-all clause
    void LandingCatchAll(Ctx& c, int selector);
    // cleanup only: the landing pad runs destructors, then _Unwind_Resume
    void LandingCleanup(Ctx& c);

    void RegisterExceptionImports();
}
