// Values for the pointers dyld would bind in the game image.
//
// Functions bind to their own __stubs entry: an indirect call through such a
// pointer reaches CallImport with the stub address, where the libc handlers
// are registered. Data symbols bind to host storage laid out like the macOS
// original. Everything else (C++ RTTI vtables, CoreFoundation constants,
// Objective-C classes) is only touched by runtime code that never runs here,
// so it gets distinct zeroed blocks.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

#include "macho_image.h"
#include "recomp_ctx.h"

namespace recomp
{
    namespace
    {
        // Guest addresses for functions only reached through bound pointers.
        constexpr uint64_t kUnboundImport = 0xFFFFFFF0;
        constexpr uint64_t kChkstk = 0xFFFFFFE0;

        uint64_t g_stackGuard;
        uint32_t g_machTaskSelf = 0x103;
        void* g_stderrp;

        // Apple's _RuneLocale up to the case maps, for the ctype macros.
        struct RuneLocale
        {
            char magic[8];
            char encoding[32];
            uint64_t sgetrune, sputrune;
            int32_t invalidRune;
            uint32_t runetype[256];
            int32_t maplower[256];
            int32_t mapupper[256];
            uint8_t rest[1024];
        };
        RuneLocale g_runeLocale;

        void InitRuneLocale()
        {
            enum : uint32_t
            {
                A = 0x100, C = 0x200, D = 0x400, G = 0x800, L = 0x1000, P = 0x2000,
                S = 0x4000, U = 0x8000, X = 0x10000, B = 0x20000, R = 0x40000,
            };
            memcpy(g_runeLocale.magic, "RuneMagA", 8);
            strcpy(g_runeLocale.encoding, "NONE");
            g_runeLocale.invalidRune = 0xFFFD;
            for (int c = 0; c < 256; c++)
            {
                uint32_t t = 0;
                if (c < 128)
                {
                    if (isalpha(c)) t |= A;
                    if (iscntrl(c)) t |= C;
                    if (isdigit(c)) t |= D | uint32_t(c - '0');
                    if (isgraph(c)) t |= G;
                    if (islower(c)) t |= L;
                    if (ispunct(c)) t |= P;
                    if (isspace(c)) t |= S;
                    if (isupper(c)) t |= U;
                    if (isxdigit(c)) t |= X | (isdigit(c) ? 0 : uint32_t(10 + tolower(c) - 'a'));
                    if (c == ' ' || c == '\t') t |= B;
                    if (isprint(c)) t |= R;
                }
                g_runeLocale.runetype[c] = t;
                g_runeLocale.maplower[c] = c < 128 ? tolower(c) : c;
                g_runeLocale.mapupper[c] = c < 128 ? toupper(c) : c;
            }
        }

        uint64_t Placeholder(const std::string& symbol)
        {
            static std::unordered_map<std::string, std::unique_ptr<uint64_t[]>> blocks;
            auto& block = blocks[symbol];
            if (!block)
                block = std::make_unique<uint64_t[]>(64);
            return reinterpret_cast<uint64_t>(block.get());
        }

        // ___chkstk_darwin probes the pages of a large frame; guest stacks are
        // committed up front, so there is nothing to touch.
        void Chkstk(Ctx&) {}

        [[noreturn]] void UnboundImport(Ctx&)
        {
            fputs("recomp: call through an unbound import pointer\n", stderr);
            abort();
        }
    }

    uint64_t ResolveImageSymbol(const MachOImage& image, const std::string& symbol)
    {
        static bool initialized = [] {
            std::random_device rd;
            g_stackGuard = (uint64_t(rd()) << 32 | rd()) & ~uint64_t(0xFF);  // keep a zero byte, like Apple's
            InitRuneLocale();
            g_stderrp = stderr;
            RegisterImport(kUnboundImport, UnboundImport);
            RegisterImport(kChkstk, Chkstk);
            return true;
        }();
        (void)initialized;

        if (symbol == "___stack_chk_guard")
            return reinterpret_cast<uint64_t>(&g_stackGuard);
        if (symbol == "__DefaultRuneLocale")
            return reinterpret_cast<uint64_t>(&g_runeLocale);
        if (symbol == "_mach_task_self_")
            return reinterpret_cast<uint64_t>(&g_machTaskSelf);
        if (symbol == "___stderrp")
            return reinterpret_cast<uint64_t>(&g_stderrp);
        if (symbol == "___chkstk_darwin")
            return image.Base() + kChkstk;
        if (uint64_t stub = image.StubAddress(symbol))
            return image.Base() + stub;
        // functions called only through pointers (personality, pure virtual, dyld)
        if (symbol == "___gxx_personality_v0" || symbol == "___cxa_pure_virtual" || symbol == "dyld_stub_binder" ||
            symbol == "_objc_msgSend")
            return image.Base() + kUnboundImport;
        return Placeholder(symbol);
    }
}
