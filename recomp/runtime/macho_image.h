#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace recomp
{
    // Loads the game's arm64 Mach-O image as data: segments are copied into one
    // host allocation and internal pointers are rebased to it. The code itself
    // is never executed; recompiled functions read the image through these
    // host addresses.
    class MachOImage
    {
    public:
        struct Segment
        {
            std::string name;
            uint64_t vmAddress;
            uint64_t vmSize;
        };

        // A pointer dyld would fill with an imported symbol's address.
        struct Binding
        {
            uint64_t vmAddress;
            std::string symbol;
            int64_t addend;
        };

        ~MachOImage();

        bool Load(const std::filesystem::path& path);

        // Writes resolve(symbol) + addend to every bound pointer (non-lazy,
        // lazy and weak). Returns the bindings resolve() returned 0 for.
        std::vector<Binding> ApplyBindings(const std::function<uint64_t(const std::string&)>& resolve);

        // Address (in image terms) of the __stubs entry calling symbol, or 0.
        uint64_t StubAddress(const std::string& symbol) const;

        uint64_t Base() const { return reinterpret_cast<uint64_t>(m_memory); }
        uint64_t Size() const { return m_size; }
        size_t RebaseCount() const { return m_rebaseCount; }
        const std::vector<Segment>& Segments() const { return m_segments; }
        const std::string& GetError() const { return m_error; }

    private:
        bool Fail(std::string message);
        bool ApplyRebases(const uint8_t* opcodes, size_t size);
        bool ParseBindings(const uint8_t* opcodes, size_t size, bool lazy);

        uint8_t* m_memory = nullptr;
        uint64_t m_size = 0;
        size_t m_rebaseCount = 0;
        std::vector<Segment> m_segments;
        std::vector<Binding> m_bindings;
        std::unordered_map<std::string, uint64_t> m_stubs;
        std::string m_error;
    };
}
