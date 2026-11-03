// Calls engine icalls reached through untyped pointers (cached icall slots,
// method pointers) with their real signatures.
//
// The generic native fallback can only pass x0..x7 as integers. Icalls with
// float parameters, float results or more than eight arguments need the
// AAPCS64 (Apple) arguments rearranged for the Windows x64 convention:
//   Apple: integers in x0..x7, floats in v0..v7, independently numbered; the
//          rest on the stack packed at natural size and alignment.
//   Win64: four positional register slots (rcx/rdx/r8/r9 or xmm0..3), the
//          rest in 8-byte stack slots.
// Signatures come from the IL2CPP metadata when the game resolves an icall
// by name (il2cpp_codegen_resolve_icall).
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "il2cpp-api.h"
#include "il2cpp-class-internals.h"
#include "il2cpp-tabledefs.h"

#include "recomp_bridge.h"
#include "recomp_ctx.h"

namespace recomp
{
    namespace
    {
        enum class Kind : uint8_t { Int, F32, F64, Void };

        struct Arg
        {
            Kind kind;
            uint8_t size;  // bytes on the Apple stack
        };

        struct Signature
        {
            std::vector<Arg> args;  // including `this`
            Kind ret = Kind::Void;
            uint8_t retSize = 8;
            bool retSigned = false;
            std::string name;
        };

        std::mutex g_lock;
        std::unordered_map<uint64_t, Signature>& Signatures()
        {
            static std::unordered_map<uint64_t, Signature> map;
            return map;
        }

        // Classifies one type; false for by-value structs (not marshaled here).
        bool Classify(const Il2CppType* t, Kind& kind, uint8_t& size, bool& isSigned)
        {
            isSigned = false;
            if (t->byref)
            {
                kind = Kind::Int, size = 8;
                return true;
            }
            switch (t->type)
            {
            case IL2CPP_TYPE_VOID: kind = Kind::Void, size = 0; return true;
            case IL2CPP_TYPE_R4: kind = Kind::F32, size = 4; return true;
            case IL2CPP_TYPE_R8: kind = Kind::F64, size = 8; return true;
            case IL2CPP_TYPE_BOOLEAN: case IL2CPP_TYPE_U1: kind = Kind::Int, size = 1; return true;
            case IL2CPP_TYPE_I1: kind = Kind::Int, size = 1, isSigned = true; return true;
            case IL2CPP_TYPE_CHAR: case IL2CPP_TYPE_U2: kind = Kind::Int, size = 2; return true;
            case IL2CPP_TYPE_I2: kind = Kind::Int, size = 2, isSigned = true; return true;
            case IL2CPP_TYPE_U4: kind = Kind::Int, size = 4; return true;
            case IL2CPP_TYPE_I4: kind = Kind::Int, size = 4, isSigned = true; return true;
            case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8: case IL2CPP_TYPE_I: case IL2CPP_TYPE_U:
            case IL2CPP_TYPE_PTR: case IL2CPP_TYPE_FNPTR:
            case IL2CPP_TYPE_STRING: case IL2CPP_TYPE_CLASS: case IL2CPP_TYPE_OBJECT:
            case IL2CPP_TYPE_SZARRAY: case IL2CPP_TYPE_ARRAY: case IL2CPP_TYPE_GENERICINST:
                if (t->type == IL2CPP_TYPE_GENERICINST && il2cpp_class_is_valuetype(il2cpp_class_from_type(t)))
                    return false;
                kind = Kind::Int, size = 8;
                return true;
            case IL2CPP_TYPE_VALUETYPE:
            {
                Il2CppClass* k = il2cpp_class_from_type(t);
                if (il2cpp_class_is_enum(k))
                    return Classify(il2cpp_class_enum_basetype(k), kind, size, isSigned);
                return false;
            }
            default:
                return false;
            }
        }

        // icall strings nest types with '/', type names may use '.' or '+'
        bool SameTypeName(const std::string& a, const char* b)
        {
            size_t n = strlen(b);
            if (a.size() != n)
                return false;
            for (size_t i = 0; i < n; i++)
            {
                char x = a[i] == '/' || a[i] == '+' ? '.' : a[i];
                char y = b[i] == '/' || b[i] == '+' ? '.' : b[i];
                if (x != y)
                    return false;
            }
            return true;
        }

        // "Ns.Outer/Inner::Method(T1,T2&)" -> the MethodInfo
        const MethodInfo* FindMethod(const std::string& icall)
        {
            size_t sep = icall.find("::"), open = icall.find('(', sep);
            if (sep == std::string::npos || open == std::string::npos)
                return nullptr;
            std::string type = icall.substr(0, sep), method = icall.substr(sep + 2, open - sep - 2);
            std::string params = icall.substr(open + 1, icall.size() - open - 2);
            std::vector<std::string> names;
            for (size_t i = 0; i < params.size();)
            {
                size_t comma = params.find(',', i);
                names.push_back(params.substr(i, comma == std::string::npos ? std::string::npos : comma - i));
                if (comma == std::string::npos)
                    break;
                i = comma + 1;
            }
            if (params.empty())
                names.clear();

            std::vector<std::string> nesting;
            for (size_t i = 0; i <= type.size();)
            {
                size_t slash = type.find('/', i);
                nesting.push_back(type.substr(i, slash == std::string::npos ? std::string::npos : slash - i));
                if (slash == std::string::npos)
                    break;
                i = slash + 1;
            }
            size_t dot = nesting[0].rfind('.');
            std::string ns = dot == std::string::npos ? "" : nesting[0].substr(0, dot);
            std::string outer = dot == std::string::npos ? nesting[0] : nesting[0].substr(dot + 1);

            size_t count = 0;
            const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
            for (size_t a = 0; a < count; a++)
            {
                Il2CppClass* k = il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[a]), ns.c_str(), outer.c_str());
                for (size_t n = 1; k && n < nesting.size(); n++)
                {
                    void* iter = nullptr;
                    Il2CppClass* nested = nullptr;
                    while ((nested = il2cpp_class_get_nested_types(k, &iter)))
                        if (nesting[n] == il2cpp_class_get_name(nested))
                            break;
                    k = nested;
                }
                if (!k)
                    continue;
                void* iter = nullptr;
                while (const MethodInfo* m = il2cpp_class_get_methods(k, &iter))
                {
                    if (method != il2cpp_method_get_name(m) || il2cpp_method_get_param_count(m) != names.size())
                        continue;
                    bool match = true;
                    for (uint32_t i = 0; i < names.size() && match; i++)
                    {
                        char* tn = il2cpp_type_get_name(il2cpp_method_get_param(m, i));
                        match = tn && SameTypeName(names[i], tn);
                        il2cpp_free(tn);
                    }
                    if (match)
                        return m;
                }
            }
            return nullptr;
        }

        // --- Win64 dynamic call: the first four positions may be float or
        // double (xmm), everything else travels as 64-bit integers ---
        template <Kind K> struct Host { using T = uint64_t; static T From(uint64_t b) { return b; } };
        template <> struct Host<Kind::F32> { using T = float; static T From(uint64_t b) { float f; uint32_t u = uint32_t(b); memcpy(&f, &u, 4); return f; } };
        template <> struct Host<Kind::F64> { using T = double; static T From(uint64_t b) { double d; memcpy(&d, &b, 8); return d; } };

        template <Kind R> struct Ret { using T = uint64_t; static uint64_t Bits(T v) { return v; } };
        template <> struct Ret<Kind::F32> { using T = float; static uint64_t Bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; } };
        template <> struct Ret<Kind::F64> { using T = double; static uint64_t Bits(double v) { uint64_t u; memcpy(&u, &v, 8); return u; } };

        constexpr int kMaxArgs = 24;
        using Invoker = uint64_t (*)(void* fn, const uint64_t* a);

        template <Kind R, Kind K0, Kind K1, Kind K2, Kind K3>
        uint64_t Invoke(void* fn, const uint64_t* a)
        {
            using F = typename Ret<R>::T (*)(typename Host<K0>::T, typename Host<K1>::T, typename Host<K2>::T, typename Host<K3>::T,
                                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            return Ret<R>::Bits(reinterpret_cast<F>(fn)(Host<K0>::From(a[0]), Host<K1>::From(a[1]), Host<K2>::From(a[2]), Host<K3>::From(a[3]),
                                                       a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15],
                                                       a[16], a[17], a[18], a[19], a[20], a[21], a[22], a[23]));
        }

        constexpr Kind kKinds[3] = { Kind::Int, Kind::F32, Kind::F64 };

        template <int I>
        constexpr Invoker Entry()
        {
            // I = ((((r * 3 + k0) * 3 + k1) * 3 + k2) * 3 + k3)
            return &Invoke<kKinds[I / 81], kKinds[(I / 27) % 3], kKinds[(I / 9) % 3], kKinds[(I / 3) % 3], kKinds[I % 3]>;
        }

        template <int... I>
        constexpr std::array<Invoker, sizeof...(I)> MakeTable(std::integer_sequence<int, I...>)
        {
            return { Entry<I>()... };
        }

        int KindIndex(Kind k) { return k == Kind::F32 ? 1 : k == Kind::F64 ? 2 : 0; }

        bool TypedCall(Ctx& c, uint64_t target)
        {
            Signature sig;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                auto it = Signatures().find(target);
                if (it == Signatures().end())
                    return false;
                sig = it->second;
            }
            if (sig.args.size() > size_t(kMaxArgs))
                return false;

            // gather the Apple arguments
            uint64_t a[kMaxArgs] = {};
            int ngrn = 0, nsrn = 0;
            uint64_t stack = c.sp;
            for (size_t i = 0; i < sig.args.size(); i++)
            {
                const Arg& arg = sig.args[i];
                bool fp = arg.kind == Kind::F32 || arg.kind == Kind::F64;
                if (fp && nsrn < 8)
                {
                    a[i] = arg.kind == Kind::F32 ? uint64_t(c.v[nsrn].u32[0]) : c.v[nsrn].u64[0];
                    nsrn++;
                }
                else if (!fp && ngrn < 8)
                    a[i] = c.x[ngrn++];
                else
                {
                    stack = (stack + arg.size - 1) & ~uint64_t(arg.size - 1);  // natural alignment
                    uint64_t v = 0;
                    memcpy(&v, reinterpret_cast<const void*>(stack), arg.size);
                    a[i] = v;
                    stack += arg.size;
                }
            }

            static const auto table = MakeTable(std::make_integer_sequence<int, 81 * 3>{});
            int index = KindIndex(sig.ret == Kind::Void ? Kind::Int : sig.ret);
            for (int i = 0; i < 4; i++)
                index = index * 3 + KindIndex(i < int(sig.args.size()) ? sig.args[i].kind : Kind::Int);

            // GOSR_TRACE_ICALLS=<text>: log calls of icalls whose name contains it
            static const char* traceFilter = [] {
                const char* v = getenv("GOSR_TRACE_ICALLS");
                return v && *v ? v : nullptr;
            }();
            if (traceFilter && sig.name.find(traceFilter) != std::string::npos)
            {
                fprintf(stderr, "icall %s(", sig.name.c_str());
                for (size_t i = 0; i < sig.args.size(); i++)
                {
                    if (sig.args[i].kind == Kind::F32)
                    {
                        float f;
                        uint32_t u = uint32_t(a[i]);
                        memcpy(&f, &u, 4);
                        fprintf(stderr, "%s%g", i ? ", " : "", f);
                    }
                    else
                        fprintf(stderr, "%s0x%llx", i ? ", " : "", static_cast<unsigned long long>(a[i]));
                }
                fputs(")\n", stderr);
                // vertex positions handed to a mesh: Mesh::SetArrayForChannelImpl(this, channel, format, dim, values, ...)
                if (sig.name.find("SetArrayForChannelImpl") != std::string::npos && a[1] == 0 && a[4])
                {
                    const float* v = reinterpret_cast<const float*>(a[4] + 0x20);
                    fputs("  positions:", stderr);
                    for (int i = 0; i < 48 && i < int(a[5]) * 3; i++)
                        fprintf(stderr, " %g", v[i]);
                    fputc('\n', stderr);
                }
            }

            SetGuestStackPointer(c.sp);
            uint64_t r = table[index](reinterpret_cast<void*>(target), a);
            if (traceFilter && sig.name.find(traceFilter) != std::string::npos)
            {
                if (sig.ret == Kind::F32)
                {
                    float f;
                    uint32_t u = uint32_t(r);
                    memcpy(&f, &u, 4);
                    fprintf(stderr, "  -> %g\n", f);
                }
                else if (sig.ret != Kind::Void)
                {
                    fprintf(stderr, "  -> 0x%llx\n", static_cast<unsigned long long>(r));
                    // byte[] results: length and first bytes
                    if (r && sig.name.find("get_bytes") != std::string::npos)
                    {
                        auto* arr = reinterpret_cast<const uint8_t*>(r);
                        uint64_t n = *reinterpret_cast<const uint64_t*>(arr + 0x18);
                        fprintf(stderr, "  bytes[%llu]:", static_cast<unsigned long long>(n));
                        for (uint64_t i = 0; i < n && i < 32; i++)
                            fprintf(stderr, " %02x", arr[0x20 + i]);
                        fputc('\n', stderr);
                    }
                }
                // *_Injected: show what the engine wrote through the last pointer argument
                if (sig.name.find("_Injected") != std::string::npos && !sig.args.empty() && a[sig.args.size() - 1])
                {
                    const float* out = reinterpret_cast<const float*>(a[sig.args.size() - 1]);
                    fprintf(stderr, "  out: %g %g %g %g\n", out[0], out[1], out[2], out[3]);
                }
            }

            switch (sig.ret)
            {
            case Kind::F32:
                c.v[0] = {};
                c.v[0].u32[0] = uint32_t(r);
                break;
            case Kind::F64:
                c.v[0] = {};
                c.v[0].u64[0] = r;
                break;
            case Kind::Int:
                // Win64 leaves the bits above a narrow result undefined
                switch (sig.retSize)
                {
                case 1: r = sig.retSigned ? uint64_t(int64_t(int8_t(r))) : uint8_t(r); break;
                case 2: r = sig.retSigned ? uint64_t(int64_t(int16_t(r))) : uint16_t(r); break;
                case 4: r = sig.retSigned ? uint64_t(int64_t(int32_t(r))) : uint32_t(r); break;
                default: break;
                }
                c.x[0] = r;
                break;
            case Kind::Void:
                break;
            }
            return true;
        }
    }

    // Records the signature of an icall the game just resolved by name.
    void RegisterIcallSignature(const char* icall, void* target)
    {
        if (!icall || !target)
            return;
        const MethodInfo* m = FindMethod(icall);
        if (!m)
        {
            fprintf(stderr, "recomp: no metadata for icall %s\n", icall);
            return;
        }
        Signature sig;
        sig.name = icall;
        if (!(il2cpp_method_get_flags(m, nullptr) & METHOD_ATTRIBUTE_STATIC))
            sig.args.push_back({ Kind::Int, 8 });
        for (uint32_t i = 0; i < il2cpp_method_get_param_count(m); i++)
        {
            Arg arg;
            bool isSigned;
            if (!Classify(il2cpp_method_get_param(m, i), arg.kind, arg.size, isSigned))
            {
                fprintf(stderr, "recomp: icall %s takes a struct by value, calling it untyped\n", icall);
                return;
            }
            sig.args.push_back(arg);
        }
        uint8_t size;
        bool isSigned;
        if (!Classify(il2cpp_method_get_return_type(m), sig.ret, size, isSigned))
        {
            fprintf(stderr, "recomp: icall %s returns a struct by value, calling it untyped\n", icall);
            return;
        }
        if (sig.args.size() > size_t(kMaxArgs))
            fprintf(stderr, "recomp: icall %s has %zu arguments, more than the typed path supports\n", icall, sig.args.size());
        sig.retSize = size;
        sig.retSigned = isSigned;
        std::lock_guard<std::mutex> guard(g_lock);
        Signatures()[reinterpret_cast<uint64_t>(target)] = std::move(sig);
        g_typedNativeCall = &TypedCall;
    }
}
