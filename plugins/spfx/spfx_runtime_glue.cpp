// The parts of the recompiler runtime the game provides and this plugin does
// not need: no entry-thunk tables, no second function table, no GC.
#include <cstdio>
#include <cstdlib>

#include "recomp_bridge.h"
#include "spfx_host.h"

namespace recomp
{
    struct BridgePart
    {
        const BridgeEntry* entries;
        size_t count;
    };
    extern const BridgePart kBridgeParts[] = { { nullptr, 0 } };
    extern const size_t kBridgePartCount = 0;
    extern const FunctionEntry kExtraFunctionTable[] = { { 0, nullptr } };
    extern const size_t kExtraFunctionCount = 0;

    void RegisterGuestStackRoot(void*, size_t) {}

    void DescribeBadCall(Ctx& c, uint64_t target)
    {
        spfx::Log("call to non-code address %p from plugin lr 0x%llx", reinterpret_cast<void*>(target),
                  static_cast<unsigned long long>(c.x[30] - g_imageBase));
    }
}
