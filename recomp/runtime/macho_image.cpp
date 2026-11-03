#include "macho_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace recomp
{
    namespace
    {
        constexpr uint32_t kMagic64 = 0xfeedfacf;
        constexpr uint32_t kCpuTypeArm64 = 0x0100000c;
        constexpr uint32_t kLcSegment64 = 0x19;
        constexpr uint32_t kLcDyldInfo = 0x22;
        constexpr uint32_t kLcDyldInfoOnly = 0x80000022;
        constexpr uint32_t kLcSymtab = 0x2;
        constexpr uint32_t kLcDysymtab = 0xb;
        constexpr uint32_t kSectionTypeMask = 0xff;
        constexpr uint32_t kSymbolStubs = 0x8;
        constexpr uint32_t kIndirectSymbolLocal = 0x80000000;
        constexpr uint32_t kIndirectSymbolAbs = 0x40000000;

        struct MachHeader64
        {
            uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
        };

        struct LoadCommand
        {
            uint32_t cmd, cmdsize;
        };

        struct SegmentCommand64
        {
            uint32_t cmd, cmdsize;
            char segname[16];
            uint64_t vmaddr, vmsize, fileoff, filesize;
            uint32_t maxprot, initprot, nsects, flags;
        };

        struct Section64
        {
            char sectname[16], segname[16];
            uint64_t addr, size;
            uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
        };

        struct SymtabCommand
        {
            uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
        };

        struct DysymtabCommand
        {
            uint32_t cmd, cmdsize;
            uint32_t ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym;
            uint32_t tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms;
            uint32_t indirectsymoff, nindirectsyms, extreloff, nextrel, locreloff, nlocrel;
        };

        struct Nlist64
        {
            uint32_t strx;
            uint8_t type, sect;
            uint16_t desc;
            uint64_t value;
        };

        struct DyldInfoCommand
        {
            uint32_t cmd, cmdsize;
            uint32_t rebase_off, rebase_size, bind_off, bind_size;
            uint32_t weak_bind_off, weak_bind_size, lazy_bind_off, lazy_bind_size;
            uint32_t export_off, export_size;
        };

        uint64_t ReadUleb(const uint8_t*& p, const uint8_t* end)
        {
            uint64_t result = 0;
            int shift = 0;
            while (p < end)
            {
                uint8_t byte = *p++;
                if (shift < 64)
                    result |= uint64_t(byte & 0x7f) << shift;
                shift += 7;
                if (!(byte & 0x80))
                    break;
            }
            return result;
        }

        int64_t ReadSleb(const uint8_t*& p, const uint8_t* end)
        {
            int64_t result = 0;
            int shift = 0;
            uint8_t byte = 0;
            while (p < end)
            {
                byte = *p++;
                if (shift < 64)
                    result |= int64_t(byte & 0x7f) << shift;
                shift += 7;
                if (!(byte & 0x80))
                    break;
            }
            if (shift < 64 && (byte & 0x40))
                result |= -(int64_t(1) << shift);
            return result;
        }

        uint8_t* AllocatePages(uint64_t size)
        {
#ifdef _WIN32
            return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
            void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
        }

        void FreePages(uint8_t* p, uint64_t size)
        {
#ifdef _WIN32
            (void)size;
            VirtualFree(p, 0, MEM_RELEASE);
#else
            munmap(p, size);
#endif
        }
    }

    MachOImage::~MachOImage()
    {
        if (m_memory)
            FreePages(m_memory, m_size);
    }

    bool MachOImage::Fail(std::string message)
    {
        m_error = std::move(message);
        return false;
    }

    bool MachOImage::Load(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return Fail("Cannot open " + path.u8string());

        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // A universal (fat) file, as shipped in the macOS app: take the arm64 slice.
        auto be32 = [&](size_t at) { return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3]; };
        if (data.size() >= 8 && be32(0) == 0xcafebabe)
        {
            uint32_t count = be32(4);
            bool found = false;
            for (uint32_t i = 0; i < count && 8 + 20 * (i + 1) <= data.size(); i++)
            {
                size_t entry = 8 + 20 * i;
                uint32_t sliceOffset = be32(entry + 8), sliceSize = be32(entry + 12);
                if (be32(entry) == kCpuTypeArm64 && uint64_t(sliceOffset) + sliceSize <= data.size())
                {
                    data = std::vector<uint8_t>(data.begin() + sliceOffset, data.begin() + sliceOffset + sliceSize);
                    found = true;
                    break;
                }
            }
            if (!found)
                return Fail("The universal image has no arm64 slice.");
        }
        if (data.size() < sizeof(MachHeader64))
            return Fail("File too small to be a Mach-O image.");

        MachHeader64 header;
        memcpy(&header, data.data(), sizeof(header));
        if (header.magic != kMagic64 || header.cputype != kCpuTypeArm64)
            return Fail("Not an arm64 Mach-O image.");

        std::vector<SegmentCommand64> segments;
        std::vector<Section64> stubSections;
        DyldInfoCommand dyldInfo{};
        SymtabCommand symtab{};
        DysymtabCommand dysymtab{};
        bool hasDyldInfo = false, hasSymtab = false, hasDysymtab = false;

        size_t offset = sizeof(MachHeader64);
        for (uint32_t i = 0; i < header.ncmds; i++)
        {
            LoadCommand lc;
            if (offset + sizeof(lc) > data.size())
                return Fail("Truncated load commands.");
            memcpy(&lc, data.data() + offset, sizeof(lc));
            if (lc.cmdsize < sizeof(lc) || offset + lc.cmdsize > data.size())
                return Fail("Malformed load command.");

            if (lc.cmd == kLcSegment64 && lc.cmdsize >= sizeof(SegmentCommand64))
            {
                SegmentCommand64 seg;
                memcpy(&seg, data.data() + offset, sizeof(seg));
                segments.push_back(seg);
                for (uint32_t k = 0; k < seg.nsects && sizeof(seg) + (k + 1) * sizeof(Section64) <= lc.cmdsize; k++)
                {
                    Section64 sect;
                    memcpy(&sect, data.data() + offset + sizeof(seg) + k * sizeof(Section64), sizeof(sect));
                    if ((sect.flags & kSectionTypeMask) == kSymbolStubs && sect.reserved2)
                        stubSections.push_back(sect);
                }
            }
            else if (lc.cmd == kLcSymtab && lc.cmdsize >= sizeof(SymtabCommand))
            {
                memcpy(&symtab, data.data() + offset, sizeof(symtab));
                hasSymtab = true;
            }
            else if (lc.cmd == kLcDysymtab && lc.cmdsize >= sizeof(DysymtabCommand))
            {
                memcpy(&dysymtab, data.data() + offset, sizeof(dysymtab));
                hasDysymtab = true;
            }
            else if ((lc.cmd == kLcDyldInfoOnly || lc.cmd == kLcDyldInfo) && lc.cmdsize >= sizeof(DyldInfoCommand))
            {
                memcpy(&dyldInfo, data.data() + offset, sizeof(dyldInfo));
                hasDyldInfo = true;
            }

            offset += lc.cmdsize;
        }

        uint64_t end = 0;
        for (const auto& seg : segments)
        {
            if (seg.fileoff + seg.filesize > data.size() || seg.filesize > seg.vmsize)
                return Fail("Segment outside the file.");
            end = std::max(end, seg.vmaddr + seg.vmsize);
        }

        m_size = (end + 0xFFFF) & ~uint64_t(0xFFFF);
        m_memory = AllocatePages(m_size);
        if (!m_memory)
            return Fail("Out of memory reserving " + std::to_string(m_size >> 20) + " MB for the image.");

        for (const auto& seg : segments)
        {
            memcpy(m_memory + seg.vmaddr, data.data() + seg.fileoff, seg.filesize);
            m_segments.push_back({ std::string(seg.segname, strnlen(seg.segname, 16)), seg.vmaddr, seg.vmsize });
        }

        if (hasDyldInfo && dyldInfo.rebase_size)
        {
            if (uint64_t(dyldInfo.rebase_off) + dyldInfo.rebase_size > data.size())
                return Fail("Rebase info outside the file.");
            if (!ApplyRebases(data.data() + dyldInfo.rebase_off, dyldInfo.rebase_size))
                return false;
        }

        if (hasDyldInfo)
        {
            struct { uint32_t off, size; bool lazy; } streams[] = {
                { dyldInfo.bind_off, dyldInfo.bind_size, false },
                { dyldInfo.weak_bind_off, dyldInfo.weak_bind_size, false },
                { dyldInfo.lazy_bind_off, dyldInfo.lazy_bind_size, true },
            };
            for (const auto& st : streams)
            {
                if (!st.size)
                    continue;
                if (uint64_t(st.off) + st.size > data.size())
                    return Fail("Bind info outside the file.");
                if (!ParseBindings(data.data() + st.off, st.size, st.lazy))
                    return false;
            }
        }

        // Symbol stubs: name -> stub address, from the indirect symbol table.
        if (hasSymtab && hasDysymtab)
        {
            for (const auto& sect : stubSections)
            {
                for (uint64_t k = 0; k < sect.size / sect.reserved2; k++)
                {
                    uint64_t slot = uint64_t(dysymtab.indirectsymoff) + 4 * (sect.reserved1 + k);
                    if (slot + 4 > data.size())
                        break;
                    uint32_t index;
                    memcpy(&index, data.data() + slot, 4);
                    if (index & (kIndirectSymbolLocal | kIndirectSymbolAbs) || index >= symtab.nsyms)
                        continue;
                    Nlist64 sym;
                    memcpy(&sym, data.data() + symtab.symoff + uint64_t(index) * sizeof(Nlist64), sizeof(sym));
                    uint64_t str = uint64_t(symtab.stroff) + sym.strx;
                    if (str >= data.size())
                        continue;
                    const char* name = reinterpret_cast<const char*>(data.data() + str);
                    m_stubs.emplace(std::string(name, strnlen(name, data.size() - str)), sect.addr + k * sect.reserved2);
                }
            }
        }

        return true;
    }

    bool MachOImage::ParseBindings(const uint8_t* p, size_t size, bool lazy)
    {
        // dyld "compressed" bind opcodes (mach-o/loader.h, BIND_OPCODE_*).
        const uint8_t* end = p + size;
        std::string symbol;
        int64_t addend = 0;
        uint64_t address = 0;

        auto bindAt = [&](uint64_t addr) -> bool
        {
            if (addr + 8 > m_size)
                return Fail("Binding outside the image.");
            m_bindings.push_back({ addr, symbol, addend });
            return true;
        };

        while (p < end)
        {
            uint8_t byte = *p++;
            uint8_t opcode = byte & 0xF0, imm = byte & 0x0F;
            switch (opcode)
            {
            case 0x00:  // DONE (lazy streams continue with the next entry)
                if (!lazy)
                    return true;
                break;
            case 0x10:  // SET_DYLIB_ORDINAL_IMM
            case 0x30:  // SET_DYLIB_SPECIAL_IMM
                break;
            case 0x20:  // SET_DYLIB_ORDINAL_ULEB
                ReadUleb(p, end);
                break;
            case 0x40:  // SET_SYMBOL_TRAILING_FLAGS_IMM
            {
                const uint8_t* s = p;
                while (p < end && *p)
                    p++;
                symbol.assign(reinterpret_cast<const char*>(s), p - s);
                if (p < end)
                    p++;
                break;
            }
            case 0x50:  // SET_TYPE_IMM (only pointer binds occur)
                break;
            case 0x60:  // SET_ADDEND_SLEB
                addend = ReadSleb(p, end);
                break;
            case 0x70:  // SET_SEGMENT_AND_OFFSET_ULEB
                if (imm >= m_segments.size())
                    return Fail("Binding in unknown segment.");
                address = m_segments[imm].vmAddress + ReadUleb(p, end);
                break;
            case 0x80:  // ADD_ADDR_ULEB
                address += ReadUleb(p, end);
                break;
            case 0x90:  // DO_BIND
                if (!bindAt(address)) return false;
                address += 8;
                break;
            case 0xA0:  // DO_BIND_ADD_ADDR_ULEB
                if (!bindAt(address)) return false;
                address += 8 + ReadUleb(p, end);
                break;
            case 0xB0:  // DO_BIND_ADD_ADDR_IMM_SCALED
                if (!bindAt(address)) return false;
                address += 8 + uint64_t(imm) * 8;
                break;
            case 0xC0:  // DO_BIND_ULEB_TIMES_SKIPPING_ULEB
            {
                uint64_t count = ReadUleb(p, end), skip = ReadUleb(p, end);
                for (uint64_t k = 0; k < count; k++)
                {
                    if (!bindAt(address)) return false;
                    address += 8 + skip;
                }
                break;
            }
            default:
                return Fail("Unknown bind opcode.");
            }
        }
        return true;
    }

    std::vector<MachOImage::Binding> MachOImage::ApplyBindings(const std::function<uint64_t(const std::string&)>& resolve)
    {
        std::vector<Binding> unresolved;
        for (const auto& b : m_bindings)
        {
            uint64_t value = resolve(b.symbol);
            if (!value)
            {
                unresolved.push_back(b);
                continue;
            }
            value += uint64_t(b.addend);
            memcpy(m_memory + b.vmAddress, &value, sizeof(value));
        }
        return unresolved;
    }

    uint64_t MachOImage::StubAddress(const std::string& symbol) const
    {
        auto it = m_stubs.find(symbol);
        return it != m_stubs.end() ? it->second : 0;
    }

    bool MachOImage::ApplyRebases(const uint8_t* p, size_t size)
    {
        // dyld "compressed" rebase opcodes (mach-o/loader.h, REBASE_OPCODE_*).
        const uint8_t* end = p + size;
        uint64_t address = 0;
        size_t segmentIndex = 0;
        uint64_t slide = Base();

        auto rebaseAt = [&](uint64_t addr) -> bool
        {
            if (segmentIndex >= m_segments.size())
                return Fail("Rebase in unknown segment.");
            uint64_t vm = m_segments[segmentIndex].vmAddress + addr;
            if (vm + 8 > m_size)
                return Fail("Rebase outside the image.");
            uint64_t value;
            memcpy(&value, m_memory + vm, 8);
            value += slide;
            memcpy(m_memory + vm, &value, 8);
            m_rebaseCount++;
            return true;
        };

        while (p < end)
        {
            uint8_t byte = *p++;
            uint8_t opcode = byte & 0xF0;
            uint8_t imm = byte & 0x0F;

            switch (opcode)
            {
            case 0x00:  // DONE
                return true;
            case 0x10:  // SET_TYPE_IMM (only pointer rebases occur)
                break;
            case 0x20:  // SET_SEGMENT_AND_OFFSET_ULEB
                segmentIndex = imm;
                address = ReadUleb(p, end);
                break;
            case 0x30:  // ADD_ADDR_ULEB
                address += ReadUleb(p, end);
                break;
            case 0x40:  // ADD_ADDR_IMM_SCALED
                address += uint64_t(imm) * 8;
                break;
            case 0x50:  // DO_REBASE_IMM_TIMES
                for (int i = 0; i < imm; i++, address += 8)
                    if (!rebaseAt(address)) return false;
                break;
            case 0x60:  // DO_REBASE_ULEB_TIMES
            {
                uint64_t count = ReadUleb(p, end);
                for (uint64_t i = 0; i < count; i++, address += 8)
                    if (!rebaseAt(address)) return false;
                break;
            }
            case 0x70:  // DO_REBASE_ADD_ADDR_ULEB
                if (!rebaseAt(address)) return false;
                address += ReadUleb(p, end) + 8;
                break;
            case 0x80:  // DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
            {
                uint64_t count = ReadUleb(p, end);
                uint64_t skip = ReadUleb(p, end);
                for (uint64_t i = 0; i < count; i++, address += skip + 8)
                    if (!rebaseAt(address)) return false;
                break;
            }
            default:
                return Fail("Unknown rebase opcode.");
            }
        }

        return true;
    }
}
