#pragma once

// Minimal MessagePack value, reader and writer for the local game server.
// Maps keep insertion order (the client's formatters look keys up by name).

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gosr::msgpack
{
    class Value
    {
    public:
        enum class Type { Nil, Bool, Int, UInt, Float, Str, Bin, Array, Map };

        Value() = default;
        Value(std::nullptr_t) {}
        Value(bool b) : m_type(Type::Bool), m_int(b) {}
        Value(int v) : m_type(Type::Int), m_int(v) {}
        Value(int64_t v) : m_type(Type::Int), m_int(v) {}
        Value(uint64_t v) : m_type(Type::UInt), m_int(int64_t(v)) {}
        Value(double v) : m_type(Type::Float), m_float(v) {}
        Value(const char* s) : m_type(Type::Str), m_str(s) {}
        Value(std::string s) : m_type(Type::Str), m_str(std::move(s)) {}

        static Value Binary(std::vector<uint8_t> b)
        {
            Value v;
            v.m_type = Type::Bin;
            v.m_bin = std::move(b);
            return v;
        }
        static Value Array() { Value v; v.m_type = Type::Array; return v; }
        static Value Map() { Value v; v.m_type = Type::Map; return v; }

        Type type() const { return m_type; }
        bool IsNil() const { return m_type == Type::Nil; }
        bool IsMap() const { return m_type == Type::Map; }
        bool IsArray() const { return m_type == Type::Array; }

        int64_t AsInt(int64_t fallback = 0) const
        {
            switch (m_type)
            {
            case Type::Bool: case Type::Int: case Type::UInt: return m_int;
            case Type::Float: return int64_t(m_float);
            default: return fallback;
            }
        }
        double AsFloat() const { return m_type == Type::Float ? m_float : double(AsInt()); }
        bool AsBool() const { return AsInt() != 0; }
        const std::string& AsString() const { return m_str; }
        const std::vector<uint8_t>& AsBinary() const { return m_bin; }

        // arrays
        std::vector<Value>& Items() { return m_items; }
        const std::vector<Value>& Items() const { return m_items; }
        Value& Push(Value v) { m_items.push_back(std::move(v)); return m_items.back(); }

        // maps
        std::vector<std::pair<std::string, Value>>& Entries() { return m_entries; }
        const std::vector<std::pair<std::string, Value>>& Entries() const { return m_entries; }
        const Value* Find(const std::string& key) const
        {
            for (const auto& e : m_entries)
                if (e.first == key)
                    return &e.second;
            return nullptr;
        }
        const Value& operator[](const std::string& key) const
        {
            static const Value nil;
            const Value* v = Find(key);
            return v ? *v : nil;
        }
        Value& Set(const std::string& key, Value v)
        {
            for (auto& e : m_entries)
                if (e.first == key)
                    return e.second = std::move(v);
            m_entries.emplace_back(key, std::move(v));
            return m_entries.back().second;
        }

        std::string Dump(int depth = 0) const;

    private:
        Type m_type = Type::Nil;
        int64_t m_int = 0;
        double m_float = 0;
        std::string m_str;
        std::vector<uint8_t> m_bin;
        std::vector<Value> m_items;
        std::vector<std::pair<std::string, Value>> m_entries;
    };

    // Decodes one value; returns false on malformed input.
    bool Decode(const uint8_t* data, size_t size, Value& out, size_t* used = nullptr);
    std::vector<uint8_t> Encode(const Value& v);
}
