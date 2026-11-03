// Runs recompiled functions against outputs recorded from the original arm64
// code in Unicorn (tools/recomp/difftest.py).
//
// usage: difftest <GameAssembly arm64 slice>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "recomp_ctx.h"
#include "macho_image.h"

namespace
{
    enum ReturnKind { kI8, kI16, kI32, kI64, kF32, kF64 };

    struct Vector
    {
        uint64_t address;
        const char* name;
        const char* kinds;  // 'i' integer / 'f' float per argument
        uint64_t args[16];
        ReturnKind ret;
        uint64_t expected;
    };

    const Vector kVectors[] = {
#include "difftest_vectors.inc"
    };

    bool Matches(ReturnKind kind, uint64_t got, uint64_t expected)
    {
        switch (kind)
        {
        case kI8: return uint8_t(got) == uint8_t(expected);
        case kI16: return uint16_t(got) == uint16_t(expected);
        case kI32: return uint32_t(got) == uint32_t(expected);
        case kI64: return got == expected;
        case kF32:
        {
            float a = recomp::BitsToF32(uint32_t(got)), b = recomp::BitsToF32(uint32_t(expected));
            return uint32_t(got) == uint32_t(expected) || (std::isnan(a) && std::isnan(b));
        }
        case kF64:
        {
            double a = recomp::BitsToF64(got), b = recomp::BitsToF64(expected);
            return got == expected || (std::isnan(a) && std::isnan(b));
        }
        }
        return false;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: %s <GameAssembly arm64 slice>\n", argv[0]);
        return 1;
    }

    recomp::MachOImage image;
    if (!image.Load(argv[1]))
    {
        printf("failed to load image: %s\n", image.GetError().c_str());
        return 1;
    }
    recomp::g_imageBase = image.Base();
    printf("image loaded at 0x%" PRIx64 ", %zu rebases\n", image.Base(), image.RebaseCount());

    constexpr size_t kStackSize = 1 << 20;
    std::vector<uint8_t> stack(kStackSize);
    auto ctx = std::make_unique<recomp::Ctx>();

    size_t passed = 0, failed = 0, missing = 0;
    uint64_t lastFailedFunction = 0;
    for (const Vector& v : kVectors)
    {
        recomp::Function fn = recomp::FindFunction(v.address);
        if (!fn)
        {
            missing++;
            continue;
        }

        recomp::Ctx& c = *ctx;
        memset(&c, 0, sizeof(c));
        size_t xi = 0, vi = 0;
        for (size_t i = 0; v.kinds[i]; i++)
        {
            if (v.kinds[i] == 'f')
                c.v[vi++].u64[0] = v.args[i];
            else
                c.x[xi++] = v.args[i];
        }
        c.x[xi] = 0;  // const MethodInfo*
        c.sp = reinterpret_cast<uint64_t>(stack.data()) + kStackSize - 0x100;

        fn(c);

        uint64_t got = (v.ret == kF32 || v.ret == kF64) ? c.v[0].u64[0] : c.x[0];
        if (Matches(v.ret, got, v.expected))
        {
            passed++;
        }
        else
        {
            failed++;
            if (lastFailedFunction != v.address && failed <= 40)
                printf("FAIL %#" PRIx64 " %s: got %#" PRIx64 " expected %#" PRIx64 "\n", v.address, v.name, got, v.expected);
            lastFailedFunction = v.address;
        }
    }

    printf("difftest: %zu passed, %zu failed, %zu without a recompiled function\n", passed, failed, missing);
    return failed == 0 ? 0 : 1;
}
