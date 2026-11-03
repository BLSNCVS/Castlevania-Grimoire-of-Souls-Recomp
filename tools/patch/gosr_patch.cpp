// Binary patches for the converted game files, so that a release ships the
// differences from the user's own files instead of converted copies of them.
//
//   gosr_patch diff  <original> <converted> <out.gpatch>   (when packaging)
//   gosr_patch apply <original> <patch.gpatch> <out>       (Setup.bat)
//
// Format (little-endian): "GOSRPAT1", u64 source size, u64 target size,
// u32 source CRC-32, u32 target CRC-32, then operations until a 0 byte:
//   1 copy: u64 source offset, u32 length
//   2 add:  u32 length, then the bytes
// Patches are stored uncompressed; the release archive compresses them.
// apply refuses a source whose size or CRC differ (another game version) and
// checks the result's CRC.
//
// diff finds matches the way rsync and xdelta do: the source is indexed by
// the hash of every kBlock-byte block at kBlock-byte steps, the target is
// scanned with a rolling hash, and candidate matches are verified and
// extended in both directions.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr size_t kBlock = 32;

    // std::filesystem paths: on Windows they come from the wide command line,
    // so folders with any characters in their names work
    using Path = std::filesystem::path;

    bool ReadFile(const Path& path, std::vector<uint8_t>& out)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return false;
        std::streamoff size = f.tellg();
        if (size < 0)
            return false;
        out.resize(size_t(size));
        f.seekg(0);
        return size == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), size));
    }

    bool WriteFile(const Path& path, const std::vector<uint8_t>& data)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        f.close();
        return bool(f);
    }

    uint32_t Crc32(const uint8_t* p, size_t n)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++)
            c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    template <typename T>
    void Put(std::vector<uint8_t>& out, T v)
    {
        uint8_t b[sizeof(T)];
        memcpy(b, &v, sizeof(T));
        out.insert(out.end(), b, b + sizeof(T));
    }

    template <typename T>
    bool Get(const std::vector<uint8_t>& in, size_t& pos, T& v)
    {
        if (pos + sizeof(T) > in.size())
            return false;
        memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    // Polynomial rolling hash over kBlock bytes
    constexpr uint64_t kBase = 1000003ull;
    uint64_t Power()
    {
        uint64_t p = 1;
        for (size_t i = 0; i < kBlock - 1; i++)
            p *= kBase;
        return p;
    }
    uint64_t Hash(const uint8_t* p)
    {
        uint64_t h = 0;
        for (size_t i = 0; i < kBlock; i++)
            h = h * kBase + p[i];
        return h;
    }

    int Diff(const Path& srcPath, const Path& dstPath, const Path& outPath)
    {
        std::vector<uint8_t> src, dst;
        if (!ReadFile(srcPath, src) || !ReadFile(dstPath, dst))
        {
            fprintf(stderr, "cannot read the input files\n");
            return 1;
        }
        std::unordered_map<uint64_t, uint64_t> index;  // block hash -> first source offset
        index.reserve(src.size() / kBlock + 1);
        for (size_t off = 0; off + kBlock <= src.size(); off += kBlock)
            index.emplace(Hash(src.data() + off), off);

        std::vector<uint8_t> out;
        out.insert(out.end(), { 'G', 'O', 'S', 'R', 'P', 'A', 'T', '1' });
        Put<uint64_t>(out, src.size());
        Put<uint64_t>(out, dst.size());
        Put<uint32_t>(out, Crc32(src.data(), src.size()));
        Put<uint32_t>(out, Crc32(dst.data(), dst.size()));

        size_t literalStart = 0, copied = 0;
        auto flushLiteral = [&](size_t end) {
            while (literalStart < end)
            {
                uint32_t n = uint32_t(std::min<size_t>(end - literalStart, 0x7FFFFFFF));
                out.push_back(2);
                Put<uint32_t>(out, n);
                out.insert(out.end(), dst.begin() + long(literalStart), dst.begin() + long(literalStart + n));
                literalStart += n;
            }
        };

        const uint64_t power = Power();
        size_t pos = 0;
        uint64_t h = dst.size() >= kBlock ? Hash(dst.data()) : 0;
        while (pos + kBlock <= dst.size())
        {
            auto it = index.find(h);
            if (it != index.end() && !memcmp(src.data() + it->second, dst.data() + pos, kBlock))
            {
                // extend backwards over pending literals, then forwards
                size_t s = it->second, d = pos;
                while (d > literalStart && s > 0 && src[s - 1] == dst[d - 1])
                    s--, d--;
                size_t len = pos + kBlock - d;
                while (d + len < dst.size() && s + len < src.size() && src[s + len] == dst[d + len])
                    len++;
                flushLiteral(d);
                for (size_t done = 0; done < len;)
                {
                    uint32_t n = uint32_t(std::min<size_t>(len - done, 0x7FFFFFFF));
                    out.push_back(1);
                    Put<uint64_t>(out, s + done);
                    Put<uint32_t>(out, n);
                    done += n;
                }
                copied += len;
                pos = d + len;
                literalStart = pos;
                if (pos + kBlock <= dst.size())
                    h = Hash(dst.data() + pos);
                continue;
            }
            if (pos + kBlock < dst.size())
                h = (h - dst[pos] * power) * kBase + dst[pos + kBlock];
            pos++;
        }
        flushLiteral(dst.size());
        out.push_back(0);
        if (!WriteFile(outPath, out))
        {
            fprintf(stderr, "cannot write %s\n", outPath.string().c_str());
            return 1;
        }
        printf("%s: %zu bytes, %zu copied from the original, patch %zu bytes\n", dstPath.string().c_str(), dst.size(), copied, out.size());
        return 0;
    }

    int Apply(const Path& srcPath, const Path& patchPath, const Path& outPath)
    {
        std::vector<uint8_t> src, patch;
        if (!ReadFile(srcPath, src))
        {
            fprintf(stderr, "cannot read %s\n", srcPath.string().c_str());
            return 1;
        }
        if (!ReadFile(patchPath, patch) || patch.size() < 32 || memcmp(patch.data(), "GOSRPAT1", 8))
        {
            fprintf(stderr, "%s is not a patch\n", patchPath.string().c_str());
            return 1;
        }
        size_t pos = 8;
        uint64_t srcSize = 0, dstSize = 0;
        uint32_t srcCrc = 0, dstCrc = 0;
        Get(patch, pos, srcSize);
        Get(patch, pos, dstSize);
        Get(patch, pos, srcCrc);
        Get(patch, pos, dstCrc);
        if (src.size() != srcSize || Crc32(src.data(), src.size()) != srcCrc)
        {
            fprintf(stderr, "%s is not the expected file (another version of the game?)\n", srcPath.string().c_str());
            return 2;
        }
        std::vector<uint8_t> dst;
        dst.reserve(size_t(dstSize));
        for (;;)
        {
            uint8_t op = 0;
            if (!Get(patch, pos, op))
                break;
            if (op == 0)
                break;
            if (op == 1)
            {
                uint64_t off = 0;
                uint32_t len = 0;
                if (!Get(patch, pos, off) || !Get(patch, pos, len) || off > src.size() || len > src.size() - off)
                    break;
                dst.insert(dst.end(), src.begin() + long(off), src.begin() + long(off + len));
            }
            else if (op == 2)
            {
                uint32_t len = 0;
                if (!Get(patch, pos, len) || len > patch.size() - pos)
                    break;
                dst.insert(dst.end(), patch.begin() + long(pos), patch.begin() + long(pos + len));
                pos += len;
            }
            else
            {
                break;
            }
        }
        if (dst.size() != dstSize || Crc32(dst.data(), dst.size()) != dstCrc)
        {
            fprintf(stderr, "%s: the patch is damaged\n", patchPath.string().c_str());
            return 3;
        }
        if (!WriteFile(outPath, dst))
        {
            fprintf(stderr, "cannot write %s\n", outPath.string().c_str());
            return 1;
        }
        return 0;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    auto is = [&](int i, const char* word) { return Path(argv[i]).string() == word; };
    if (argc == 5 && is(1, "diff"))
        return Diff(argv[2], argv[3], argv[4]);
    if (argc == 5 && is(1, "apply"))
        return Apply(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: gosr_patch diff <original> <converted> <out.gpatch>\n"
                    "       gosr_patch apply <original> <patch.gpatch> <out>\n");
    return 1;
}
