#pragma once

// Calls native functions with arguments taken from the guest registers,
// following AAPCS64 for integer, pointer and floating point parameters.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#include "recomp_bridge.h"
#include "recomp_ctx.h"

namespace recomp::native
{
    template <typename T>
    T Arg(const recomp::Ctx& c, int i) { return (T)(c.x[i]); }

    inline void Enter(recomp::Ctx& c) { recomp::SetGuestStackPointer(c.sp); }

    // Converts a guest integer register to a native parameter of type T.
    template <typename T>
    T FromReg(uint64_t v)
    {
        if constexpr (std::is_same_v<T, bool>)
            return (v & 0xff) != 0;
        else if constexpr (std::is_reference_v<T>)
            return *reinterpret_cast<std::remove_reference_t<T>*>(v);
        else if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(v);
        else
            return static_cast<T>(v);
    }

    template <typename R>
    uint64_t ToReg(R v)
    {
        if constexpr (std::is_same_v<R, bool>)
            return v ? 1 : 0;
        else if constexpr (std::is_pointer_v<R>)
            return reinterpret_cast<uint64_t>(v);
        else
            return static_cast<uint64_t>(v);
    }

    // Integer registers a parameter occupies: aggregates up to 16 bytes travel
    // in consecutive x registers, larger ones as a pointer to a copy.
    template <typename T>
    constexpr int IntRegs()
    {
        if constexpr (std::is_class_v<T>)
            return sizeof(T) <= 16 ? int((sizeof(T) + 7) / 8) : 1;
        else
            return 1;
    }

    // AAPCS64 register of each parameter: integers, pointers and aggregates
    // take x0-x7, floating point v0-v7, each class counted separately.
    template <typename... A>
    struct ArgRegs
    {
        static constexpr bool kFloat[sizeof...(A) + 1] = { std::is_floating_point_v<A>..., false };
        static constexpr int kRegs[sizeof...(A) + 1] = { IntRegs<A>()..., 0 };

        static constexpr int Index(size_t i)
        {
            int n = 0;
            for (size_t k = 0; k < i; k++)
            {
                if (kFloat[k] == kFloat[i])
                    n += kFloat[i] ? 1 : kRegs[k];
            }
            return n;
        }

        static constexpr bool Fits()
        {
            for (size_t i = 0; i < sizeof...(A); i++)
            {
                if (Index(i) + (kFloat[i] ? 1 : kRegs[i]) > 8)
                    return false;
            }
            return true;
        }
    };

    template <typename T, size_t I, typename Regs>
    T GetArg(const recomp::Ctx& c)
    {
        if constexpr (std::is_same_v<T, float>)
            return c.v[Regs::Index(I)].f32[0];
        else if constexpr (std::is_same_v<T, double>)
            return c.v[Regs::Index(I)].f64[0];
        else if constexpr (std::is_class_v<T>)
        {
            static_assert(std::is_trivially_copyable_v<T>, "aggregate parameters must be plain data");
            T v;
            if constexpr (sizeof(T) <= 16)
                memcpy(&v, &c.x[Regs::Index(I)], sizeof(T));
            else
                memcpy(&v, reinterpret_cast<const void*>(c.x[Regs::Index(I)]), sizeof(T));
            return v;
        }
        else
            return FromReg<T>(c.x[Regs::Index(I)]);
    }

    template <typename R>
    void SetResult(recomp::Ctx& c, R v)
    {
        if constexpr (std::is_same_v<R, float>)
            recomp::SetS(c.v[0], v);
        else if constexpr (std::is_same_v<R, double>)
            recomp::SetD(c.v[0], v);
        else if constexpr (std::is_class_v<R>)
        {
            static_assert(std::is_trivially_copyable_v<R>, "aggregate results must be plain data");
            if constexpr (sizeof(R) <= 16)
                memcpy(&c.x[0], &v, sizeof(R));
            else
                memcpy(reinterpret_cast<void*>(c.x[8]), &v, sizeof(R));
        }
        else
            c.x[0] = ToReg<R>(v);
    }

    // Handler calling native F with arguments from the guest registers.
    template <auto F>
    struct Native;

    template <typename R, typename... A, R (*F)(A...)>
    struct Native<F>
    {
        using Regs = ArgRegs<A...>;
        static_assert(Regs::Fits(), "stack parameters need a hand-written handler");

        template <size_t... I>
        static void Call(recomp::Ctx& c, std::index_sequence<I...>)
        {
            Enter(c);
            if constexpr (std::is_void_v<R>)
                F(GetArg<A, I, Regs>(c)...);
            else
                SetResult<R>(c, F(GetArg<A, I, Regs>(c)...));
        }

        static void Handler(recomp::Ctx& c) { Call(c, std::index_sequence_for<A...>{}); }
    };

    template <auto F>
    constexpr recomp::Function N = &Native<F>::Handler;

}
