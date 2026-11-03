#include "msgpack.h"

#include <cstdio>

namespace gosr::msgpack
{
    namespace
    {
        struct Reader
        {
            const uint8_t* p;
            const uint8_t* end;
            bool ok = true;

            bool Need(size_t n)
            {
                if (size_t(end - p) < n)
                    ok = false;
                return ok;
            }
            uint64_t Big(int n)
            {
                if (!Need(n))
                    return 0;
                uint64_t v = 0;
                for (int i = 0; i < n; i++)
                    v = (v << 8) | *p++;
                return v;
            }
            std::string Str(size_t n)
            {
                if (!Need(n))
                    return {};
                std::string s(reinterpret_cast<const char*>(p), n);
                p += n;
                return s;
            }

            Value Read(int depth)
            {
                if (depth > 64 || !Need(1))
                {
                    ok = false;
                    return {};
                }
                uint8_t b = *p++;
                if (b <= 0x7f) return Value(int64_t(b));
                if (b >= 0xe0) return Value(int64_t(int8_t(b)));
                if ((b & 0xe0) == 0xa0) return Value(Str(b & 0x1f));
                if ((b & 0xf0) == 0x90) return ReadArray(b & 0x0f, depth);
                if ((b & 0xf0) == 0x80) return ReadMap(b & 0x0f, depth);
                switch (b)
                {
                case 0xc0: return {};
                case 0xc2: return Value(false);
                case 0xc3: return Value(true);
                case 0xc4: return Bin(Big(1));
                case 0xc5: return Bin(Big(2));
                case 0xc6: return Bin(Big(4));
                case 0xca: { uint32_t u = uint32_t(Big(4)); float f; memcpy(&f, &u, 4); return Value(double(f)); }
                case 0xcb: { uint64_t u = Big(8); double d; memcpy(&d, &u, 8); return Value(d); }
                case 0xcc: return Value(int64_t(Big(1)));
                case 0xcd: return Value(int64_t(Big(2)));
                case 0xce: return Value(int64_t(Big(4)));
                case 0xcf: return Value(uint64_t(Big(8)));
                case 0xd0: return Value(int64_t(int8_t(Big(1))));
                case 0xd1: return Value(int64_t(int16_t(Big(2))));
                case 0xd2: return Value(int64_t(int32_t(Big(4))));
                case 0xd3: return Value(int64_t(Big(8)));
                case 0xd9: return Value(Str(Big(1)));
                case 0xda: return Value(Str(Big(2)));
                case 0xdb: return Value(Str(Big(4)));
                case 0xdc: return ReadArray(Big(2), depth);
                case 0xdd: return ReadArray(Big(4), depth);
                case 0xde: return ReadMap(Big(2), depth);
                case 0xdf: return ReadMap(Big(4), depth);
                default:
                    // ext types are not used by the game's formatters
                    ok = false;
                    return {};
                }
            }
            Value Bin(size_t n)
            {
                if (!Need(n))
                    return {};
                Value v = Value::Binary(std::vector<uint8_t>(p, p + n));
                p += n;
                return v;
            }
            Value ReadArray(uint64_t n, int depth)
            {
                Value v = Value::Array();
                for (uint64_t i = 0; i < n && ok; i++)
                    v.Push(Read(depth + 1));
                return v;
            }
            Value ReadMap(uint64_t n, int depth)
            {
                Value v = Value::Map();
                for (uint64_t i = 0; i < n && ok; i++)
                {
                    Value k = Read(depth + 1);
                    Value val = Read(depth + 1);
                    std::string key = k.type() == Value::Type::Str ? k.AsString() : std::to_string(k.AsInt());
                    v.Entries().emplace_back(std::move(key), std::move(val));
                }
                return v;
            }
        };

        void Big(std::vector<uint8_t>& o, uint64_t v, int n)
        {
            for (int i = n - 1; i >= 0; i--)
                o.push_back(uint8_t(v >> (8 * i)));
        }

        void Write(std::vector<uint8_t>& o, const Value& v)
        {
            switch (v.type())
            {
            case Value::Type::Nil: o.push_back(0xc0); break;
            case Value::Type::Bool: o.push_back(v.AsBool() ? 0xc3 : 0xc2); break;
            case Value::Type::Int:
            case Value::Type::UInt:
            {
                int64_t i = v.AsInt();
                if (v.type() == Value::Type::UInt && i < 0) { o.push_back(0xcf); Big(o, uint64_t(i), 8); }
                else if (i >= 0 && i <= 0x7f) o.push_back(uint8_t(i));
                else if (i < 0 && i >= -32) o.push_back(uint8_t(int8_t(i)));
                else if (i >= 0 && i <= 0xff) { o.push_back(0xcc); Big(o, i, 1); }
                else if (i >= 0 && i <= 0xffff) { o.push_back(0xcd); Big(o, i, 2); }
                else if (i >= 0 && i <= 0xffffffffll) { o.push_back(0xce); Big(o, i, 4); }
                else if (i >= -128 && i < 0) { o.push_back(0xd0); Big(o, uint64_t(i), 1); }
                else if (i >= -32768 && i < 0) { o.push_back(0xd1); Big(o, uint64_t(i), 2); }
                else if (i >= INT32_MIN && i < 0) { o.push_back(0xd2); Big(o, uint64_t(i), 4); }
                else { o.push_back(0xd3); Big(o, uint64_t(i), 8); }
                break;
            }
            case Value::Type::Float:
            {
                double d = v.AsFloat();
                uint64_t u;
                memcpy(&u, &d, 8);
                o.push_back(0xcb);
                Big(o, u, 8);
                break;
            }
            case Value::Type::Str:
            {
                const std::string& s = v.AsString();
                size_t n = s.size();
                if (n < 32) o.push_back(uint8_t(0xa0 | n));
                else if (n <= 0xff) { o.push_back(0xd9); Big(o, n, 1); }
                else if (n <= 0xffff) { o.push_back(0xda); Big(o, n, 2); }
                else { o.push_back(0xdb); Big(o, n, 4); }
                o.insert(o.end(), s.begin(), s.end());
                break;
            }
            case Value::Type::Bin:
            {
                const auto& b = v.AsBinary();
                size_t n = b.size();
                if (n <= 0xff) { o.push_back(0xc4); Big(o, n, 1); }
                else if (n <= 0xffff) { o.push_back(0xc5); Big(o, n, 2); }
                else { o.push_back(0xc6); Big(o, n, 4); }
                o.insert(o.end(), b.begin(), b.end());
                break;
            }
            case Value::Type::Array:
            {
                size_t n = v.Items().size();
                if (n < 16) o.push_back(uint8_t(0x90 | n));
                else if (n <= 0xffff) { o.push_back(0xdc); Big(o, n, 2); }
                else { o.push_back(0xdd); Big(o, n, 4); }
                for (const auto& it : v.Items())
                    Write(o, it);
                break;
            }
            case Value::Type::Map:
            {
                size_t n = v.Entries().size();
                if (n < 16) o.push_back(uint8_t(0x80 | n));
                else if (n <= 0xffff) { o.push_back(0xde); Big(o, n, 2); }
                else { o.push_back(0xdf); Big(o, n, 4); }
                for (const auto& e : v.Entries())
                {
                    Write(o, Value(e.first));
                    Write(o, e.second);
                }
                break;
            }
            }
        }
    }

    bool Decode(const uint8_t* data, size_t size, Value& out, size_t* used)
    {
        Reader r{ data, data + size };
        out = r.Read(0);
        if (used)
            *used = size_t(r.p - data);
        return r.ok;
    }

    std::vector<uint8_t> Encode(const Value& v)
    {
        std::vector<uint8_t> o;
        Write(o, v);
        return o;
    }

    std::string Value::Dump(int depth) const
    {
        std::string pad(depth * 2, ' ');
        char buf[64];
        switch (m_type)
        {
        case Type::Nil: return "nil";
        case Type::Bool: return m_int ? "true" : "false";
        case Type::Int: return std::to_string(m_int);
        case Type::UInt: return std::to_string(uint64_t(m_int));
        case Type::Float: snprintf(buf, sizeof(buf), "%g", m_float); return buf;
        case Type::Str: return "\"" + m_str + "\"";
        case Type::Bin: return "<" + std::to_string(m_bin.size()) + " bytes>";
        case Type::Array:
        {
            std::string s = "[";
            for (size_t i = 0; i < m_items.size(); i++)
                s += (i ? ", " : "") + m_items[i].Dump(depth + 1);
            return s + "]";
        }
        case Type::Map:
        {
            std::string s = "{";
            for (size_t i = 0; i < m_entries.size(); i++)
                s += (i ? ", " : "") + m_entries[i].first + ": " + m_entries[i].second.Dump(depth + 1);
            return s + "}";
        }
        }
        return "?";
    }
}
