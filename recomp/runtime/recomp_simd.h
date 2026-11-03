#pragma once

// Lane-wise helpers for the AdvSIMD instructions the recompiler emits. Each
// operation computes a fresh result vector, so lanes beyond the operation's
// width (e.g. the upper half for 64-bit forms) are zero, as on arm64.

#include <type_traits>

#include "recomp_ctx.h"

namespace recomp
{
    template <typename T>
    inline T& Lane(V128& v, int i) { return reinterpret_cast<T*>(v.u8)[i]; }

    template <typename T>
    inline T Lane(const V128& v, int i) { T value; memcpy(&value, v.u8 + i * sizeof(T), sizeof(T)); return value; }

    template <typename T>
    inline T AllOnes() { return T(~T(0)); }

    template <typename T, int N, typename F>
    inline void Map1(V128& d, const V128& a, F f)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            Lane<T>(r, i) = T(f(Lane<T>(a, i)));
        d = r;
    }

    template <typename T, int N, typename F>
    inline void Map2(V128& d, const V128& a, const V128& b, F f)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            Lane<T>(r, i) = T(f(Lane<T>(a, i), Lane<T>(b, i)));
        d = r;
    }

    // Second operand is one lane of b, broadcast (by-element forms).
    template <typename T, int N, typename F>
    inline void Map2Elem(V128& d, const V128& a, const V128& b, int index, F f)
    {
        V128 r{};
        T y = Lane<T>(b, index);
        for (int i = 0; i < N; i++)
            Lane<T>(r, i) = T(f(Lane<T>(a, i), y));
        d = r;
    }

    template <typename T, int N, typename F>
    inline void Map3(V128& d, const V128& a, const V128& b, const V128& c, F f)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            Lane<T>(r, i) = T(f(Lane<T>(a, i), Lane<T>(b, i), Lane<T>(c, i)));
        d = r;
    }

    template <typename T, int N>
    inline void Fill(V128& d, T value)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            Lane<T>(r, i) = value;
        d = r;
    }

    // Shifts whose amount may reach the lane width (ushr/sshr #64 are legal).
    template <typename T>
    inline T Lsr(T x, int s) { return s >= int(sizeof(T) * 8) ? T(0) : T(x >> s); }

    template <typename T>
    inline T Asr(T x, int s)
    {
        using S = std::make_signed_t<T>;
        int bits = int(sizeof(T) * 8);
        return T(S(x) >> (s >= bits ? bits - 1 : s));
    }

    template <typename T>
    inline T Lsl(T x, int s) { return s >= int(sizeof(T) * 8) ? T(0) : T(x << s); }

    // ushl/sshl: per-lane shift by the signed low byte of the shift lane.
    template <typename T>
    inline T Ushl(T x, T y)
    {
        int s = int8_t(y & 0xff);
        return s >= 0 ? Lsl(x, s) : Lsr(x, -s);
    }

    template <typename T>
    inline T Sshl(T x, T y)
    {
        int s = int8_t(y & 0xff);
        return s >= 0 ? Lsl(x, s) : Asr(x, -s);
    }

    template <typename T> inline auto AsFloat(T bits)
    {
        if constexpr (sizeof(T) == 4) return BitsToF32(uint32_t(bits));
        else return BitsToF64(uint64_t(bits));
    }

    template <typename T, typename F> inline T FromFloat(F value)
    {
        if constexpr (sizeof(T) == 4) return T(F32ToBits(float(value)));
        else return T(F64ToBits(double(value)));
    }

    // Across-lanes reductions into a scalar register.
    template <typename T, int N>
    inline void Addv(V128& d, const V128& a)
    {
        T sum = 0;
        for (int i = 0; i < N; i++)
            sum = T(sum + Lane<T>(a, i));
        d = V128{};
        Lane<T>(d, 0) = sum;
    }

    template <typename T, int N, bool Max>
    inline void MinMaxV(V128& d, const V128& a)
    {
        T best = Lane<T>(a, 0);
        for (int i = 1; i < N; i++)
        {
            T v = Lane<T>(a, i);
            if (Max ? v > best : v < best)
                best = v;
        }
        d = V128{};
        Lane<T>(d, 0) = best;
    }

    // Pairwise add over the concatenation a:b.
    template <typename T, int N>
    inline void Addp(V128& d, const V128& a, const V128& b)
    {
        V128 r{};
        for (int i = 0; i < N / 2; i++)
        {
            Lane<T>(r, i) = T(Lane<T>(a, 2 * i) + Lane<T>(a, 2 * i + 1));
            Lane<T>(r, i + N / 2) = T(Lane<T>(b, 2 * i) + Lane<T>(b, 2 * i + 1));
        }
        d = r;
    }

    template <typename T, int N>
    inline void Faddp(V128& d, const V128& a, const V128& b)
    {
        V128 r{};
        for (int i = 0; i < N / 2; i++)
        {
            Lane<T>(r, i) = FromFloat<T>(AsFloat(Lane<T>(a, 2 * i)) + AsFloat(Lane<T>(a, 2 * i + 1)));
            Lane<T>(r, i + N / 2) = FromFloat<T>(AsFloat(Lane<T>(b, 2 * i)) + AsFloat(Lane<T>(b, 2 * i + 1)));
        }
        d = r;
    }

    // Permutes: uzp/zip/trn over a:b.
    template <typename T, int N, int Part>
    inline void Uzp(V128& d, const V128& a, const V128& b)
    {
        V128 r{};
        for (int i = 0; i < N / 2; i++)
        {
            Lane<T>(r, i) = Lane<T>(a, 2 * i + Part);
            Lane<T>(r, i + N / 2) = Lane<T>(b, 2 * i + Part);
        }
        d = r;
    }

    template <typename T, int N, int Part>
    inline void Zip(V128& d, const V128& a, const V128& b)
    {
        V128 r{};
        int base = Part * N / 2;
        for (int i = 0; i < N / 2; i++)
        {
            Lane<T>(r, 2 * i) = Lane<T>(a, base + i);
            Lane<T>(r, 2 * i + 1) = Lane<T>(b, base + i);
        }
        d = r;
    }

    template <typename T, int N, int Part>
    inline void Trn(V128& d, const V128& a, const V128& b)
    {
        V128 r{};
        for (int i = 0; i < N / 2; i++)
        {
            Lane<T>(r, 2 * i) = Lane<T>(a, 2 * i + Part);
            Lane<T>(r, 2 * i + 1) = Lane<T>(b, 2 * i + Part);
        }
        d = r;
    }

    // tbl: byte lookup into 1-4 consecutive table registers; out of range -> 0.
    template <int N>
    inline void Tbl(V128& d, const V128* table, int tableRegs, const V128& index)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
        {
            int k = index.u8[i];
            r.u8[i] = k < tableRegs * 16 ? table[k / 16].u8[k % 16] : 0;
        }
        d = r;
    }

    // Widening: lanes of type From from the low (Part 0) or high (Part 1) half.
    template <typename To, typename From, int N, int Part>
    inline void Widen(V128& d, const V128& a, int shift)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            Lane<To>(r, i) = To(To(Lane<From>(a, Part * N + i)) << shift);
        d = r;
    }

    template <typename To, typename From, int N, int Part>
    inline void AddWide(V128& d, const V128& a, const V128& b)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            Lane<To>(r, i) = To(Lane<To>(a, i) + To(Lane<From>(b, Part * N + i)));
        d = r;
    }

    template <int N, int Part>
    inline void Fcvtl(V128& d, const V128& a)
    {
        V128 r{};
        for (int i = 0; i < N; i++)
            r.f64[i] = double(a.f32[Part * N + i]);
        d = r;
    }

    // udot: each 32-bit lane accumulates four byte products.
    template <int N>
    inline void Udot(V128& d, const V128& a, const V128& b, int elementIndex)
    {
        V128 r = d;
        for (int i = 0; i < N; i++)
        {
            int bi = elementIndex >= 0 ? elementIndex : i;
            uint32_t sum = 0;
            for (int j = 0; j < 4; j++)
                sum += uint32_t(a.u8[4 * i + j]) * uint32_t(b.u8[4 * bi + j]);
            r.u32[i] = d.u32[i] + sum;
        }
        if (N == 2)
            r.u64[1] = 0;
        d = r;
    }

    // Structure loads/stores: ldN/stN interleave lanes across k registers.
    template <typename T, int N>
    inline void LdN(Ctx& c, int firstReg, int regs, uint64_t address)
    {
        V128 r[4] = {};
        for (int i = 0; i < N; i++)
            for (int k = 0; k < regs; k++)
                Lane<T>(r[k], i) = Load<T>(address + (uint64_t(i) * regs + k) * sizeof(T));
        for (int k = 0; k < regs; k++)
            c.v[(firstReg + k) % 32] = r[k];
    }

    template <typename T, int N>
    inline void StN(const Ctx& c, int firstReg, int regs, uint64_t address)
    {
        for (int i = 0; i < N; i++)
            for (int k = 0; k < regs; k++)
                Store<T>(address + (uint64_t(i) * regs + k) * sizeof(T), Lane<T>(c.v[(firstReg + k) % 32], i));
    }
}
