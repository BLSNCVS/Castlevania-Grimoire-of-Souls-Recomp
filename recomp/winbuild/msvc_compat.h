#pragma once

// Force-included when building Unity's libil2cpp with a current MSVC STL.

// libil2cpp 2020.3 predates the STL's minimum clang version check.
#ifndef _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#endif

#ifdef __cplusplus
#include <cstddef>
#include <functional>

// stdext::hash_compare (removed from newer MSVC STLs) is the default hasher
// of the bundled Google sparsehash in its MSVC configuration.
namespace stdext
{
    template <class Key, class Less = std::less<Key>>
    class hash_compare
    {
    public:
        enum { bucket_size = 4, min_buckets = 8 };
        size_t operator()(const Key& key) const { return std::hash<Key>()(key); }
        bool operator()(const Key& a, const Key& b) const { return Less()(a, b); }
    };
}
#endif
