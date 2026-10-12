// Wings core - Compact JSON object builder that mirrors JSON.stringify output.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pegasus web UI builds every request body with JSON.stringify({...}):
// compact (no whitespace), keys in insertion order. Tests compare bodies
// byte-for-byte, so this builder emits exactly that shape.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "wings/json.hpp"

namespace wings::json
{

class ObjectBuilder
{
  public:
    ObjectBuilder &str(std::string_view key, std::string_view value)
    {
        key_(key);
        out_ += quote(value);
        return *this;
    }
    ObjectBuilder &num(std::string_view key, std::int64_t value)
    {
        key_(key);
        out_ += std::to_string(value);
        return *this;
    }
    ObjectBuilder &boolean(std::string_view key, bool value)
    {
        key_(key);
        out_ += value ? "true" : "false";
        return *this;
    }
    ObjectBuilder &raw(std::string_view key, std::string_view json_text)
    {
        key_(key);
        out_ += json_text;
        return *this;
    }
    ObjectBuilder &str_array(std::string_view key, const std::vector<std::string> &values)
    {
        key_(key);
        out_ += '[';
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
                out_ += ',';
            out_ += quote(values[i]);
        }
        out_ += ']';
        return *this;
    }
    ObjectBuilder &int_array(std::string_view key, const std::vector<std::int64_t> &values)
    {
        key_(key);
        out_ += '[';
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0)
                out_ += ',';
            out_ += std::to_string(values[i]);
        }
        out_ += ']';
        return *this;
    }
    std::string done() const
    {
        return out_ + "}";
    }

  private:
    void key_(std::string_view key)
    {
        if (!first_)
            out_ += ',';
        first_ = false;
        out_ += quote(key);
        out_ += ':';
    }
    std::string out_ = "{";
    bool first_ = true;
};

// Re-serialises a parsed document (used by the replay transport to unwrap
// recorded samples). Integers print without a fraction.
inline std::string dump(const Value &v)
{
    switch (v.type())
    {
    case Type::null:
        return "null";
    case Type::boolean:
        return v.as_bool() ? "true" : "false";
    case Type::number:
    {
        const double d = v.as_number();
        char buf[40];
        if (d == static_cast<double>(static_cast<long long>(d)) && d > -9e15 && d < 9e15)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        else
            std::snprintf(buf, sizeof(buf), "%.17g", d);
        return buf;
    }
    case Type::string:
        return quote(v.as_string());
    case Type::array:
    {
        std::string out = "[";
        bool first = true;
        for (const Value &item : v.items())
        {
            if (!first)
                out += ',';
            first = false;
            out += dump(item);
        }
        return out + "]";
    }
    case Type::object:
    {
        std::string out = "{";
        bool first = true;
        for (const std::string &key : v.keys())
        {
            if (!first)
                out += ',';
            first = false;
            out += quote(key) + ":" + dump(v[key]);
        }
        return out + "}";
    }
    }
    return "null";
}

} // namespace wings::json
