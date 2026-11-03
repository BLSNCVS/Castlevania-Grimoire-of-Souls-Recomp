#pragma once
// Host side of the recompiled SPFX plugin: loads the macOS plugin image,
// binds its imports and runs guest code on the calling thread.
#include <cstdint>
#include <string>

#include "recomp_ctx.h"

namespace spfx
{
    // Loads and binds the plugin image once; every export calls it first.
    // False when the macOS plugin binary is missing: exports then answer
    // like the game's SPFXPluginNull (no effects).
    bool EnsureLoaded();
    void* FallbackHandle();

    // Handlers for imported functions, by Mach-O symbol name ("_malloc").
    void RegisterHandler(const std::string& symbol, recomp::Function handler);

    // Runs a guest function (host address inside the image) on this thread.
    uint64_t CallGuest(uint64_t hostAddress, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0);

    // A host address the guest can call that runs handler (for function
    // pointers handed to guest code).
    uint64_t GuestCallable(recomp::Function handler);

    void Log(const char* format, ...);

    // New threads (spfx_libc.cpp): a sleep while one is starting ends when it has
    void NoteThreadStarted();
    bool WaitForThreadStart(uint32_t ms);

    // Logs effects that take long to load (spfx_host.cpp)
    void NoteInstanceCreated(void* handle, const char* file);
    void NoteInstanceStatus(void* handle, int32_t status);

    // GOSR_SPFX_TRACE: log every call the game makes into the plugin
    bool TraceCalls();

    // Registration of the import groups (each in its own file).
    void RegisterLibcImports();
    void RegisterCxxImports();
    void RegisterGlImports();
}
