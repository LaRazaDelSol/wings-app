// Wings TV app - A small JSON reader and string quoting for the Orbit API.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The backend's replies are small, trusted-shape documents (a catalogue of a
// few hundred entries at most). A DOM with linear member lookup is all the
// app needs. Parsing is bounded: nesting deeper than kMaxDepth is rejected.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::json
{

enum class Type : std::uint8_t
{
    null,
    boolean,
    number,
    string,
    array,
    object,
};

constexpr int kMaxDepth = 64;

class Value
{
  public:
    Type type() const
    {
        return type_;
    }
    bool is_null() const
    {
        return type_ == Type::null;
    }
    bool is_object() const
    {
        return type_ == Type::object;
    }
    bool is_array() const
    {
        return type_ == Type::array;
    }
    bool is_string() const
    {
        return type_ == Type::string;
    }
    bool is_number() const
    {
        return type_ == Type::number;
    }
    bool is_bool() const
    {
        return type_ == Type::boolean;
    }

    // Typed reads with a fallback for any other type.
    bool as_bool(bool fallback = false) const;
    double as_number(double fallback = 0.0) const;
    std::int64_t as_int(std::int64_t fallback = 0) const;
    // The empty string for anything that is not a string.
    const std::string &as_string() const;

    // Object member, or a null value when absent (or when this is no object).
    const Value &operator[](std::string_view key) const;
    bool has(std::string_view key) const;
    // Shorthands for members.
    const std::string &text(std::string_view key) const
    {
        return (*this)[key].as_string();
    }
    double number(std::string_view key, double fallback = 0.0) const
    {
        return (*this)[key].as_number(fallback);
    }
    bool flag(std::string_view key, bool fallback = false) const
    {
        return (*this)[key].as_bool(fallback);
    }

    // Array items (empty for anything else). Object values are not exposed
    // this way; use keys() with operator[].
    const std::vector<Value> &items() const;
    const std::vector<std::string> &keys() const
    {
        return keys_;
    }
    std::size_t size() const
    {
        return type_ == Type::array || type_ == Type::object ? items_.size() : 0;
    }

  private:
    friend class Parser;
    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;      // array items, or object member values
    std::vector<std::string> keys_; // object member names, parallel to items_
};

// Parses one complete document. On failure returns false and, when error is
// given, a short reason with the byte offset.
bool parse(std::string_view text, Value *out, std::string *error = nullptr);

// A JSON string literal, quotes included, for building request bodies.
std::string quote(std::string_view value);

} // namespace orbit::json
