// Wings TV app - A small JSON reader and string quoting for the Pegasus DL API.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/json.hpp"

#include <cmath>
#include <cstdio>

namespace orbit::json
{

namespace
{

const Value &null_value()
{
    static const Value value;
    return value;
}

const std::string &empty_string()
{
    static const std::string value;
    return value;
}

const std::vector<Value> &empty_items()
{
    static const std::vector<Value> value;
    return value;
}

void append_utf8(std::string &out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else if (code < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

} // namespace

bool Value::as_bool(bool fallback) const
{
    return type_ == Type::boolean ? bool_ : fallback;
}

double Value::as_number(double fallback) const
{
    return type_ == Type::number ? number_ : fallback;
}

std::int64_t Value::as_int(std::int64_t fallback) const
{
    if (type_ != Type::number || !std::isfinite(number_) || number_ > 9.0e18 || number_ < -9.0e18)
        return fallback;
    return static_cast<std::int64_t>(number_);
}

const std::string &Value::as_string() const
{
    return type_ == Type::string ? string_ : empty_string();
}

const Value &Value::operator[](std::string_view key) const
{
    if (type_ != Type::object)
        return null_value();
    for (std::size_t i = 0; i < keys_.size(); ++i)
    {
        if (keys_[i] == key)
            return items_[i];
    }
    return null_value();
}

bool Value::has(std::string_view key) const
{
    if (type_ != Type::object)
        return false;
    for (const std::string &name : keys_)
    {
        if (name == key)
            return true;
    }
    return false;
}

const std::vector<Value> &Value::items() const
{
    return type_ == Type::array ? items_ : empty_items();
}

class Parser
{
  public:
    explicit Parser(std::string_view text) : text_(text)
    {
    }

    bool document(Value *out)
    {
        skip_space();
        if (!value(out, 0))
            return false;
        skip_space();
        if (at_ < text_.size())
            return fail("unexpected data after the document");
        return true;
    }

    const std::string &error() const
    {
        return error_;
    }

  private:
    bool fail(const char *reason)
    {
        if (error_.empty())
        {
            char line[96];
            std::snprintf(line, sizeof(line), "%s at byte %zu", reason, at_);
            error_ = line;
        }
        return false;
    }

    void skip_space()
    {
        while (at_ < text_.size())
        {
            const char c = text_[at_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++at_;
        }
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(at_, word.size()) != word)
            return fail("invalid literal");
        at_ += word.size();
        return true;
    }

    bool value(Value *out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("nesting too deep");
        if (at_ >= text_.size())
            return fail("unexpected end");
        const char c = text_[at_];
        switch (c)
        {
        case '{':
            return object(out, depth);
        case '[':
            return array(out, depth);
        case '"':
            out->type_ = Type::string;
            return string(&out->string_);
        case 't':
            out->type_ = Type::boolean;
            out->bool_ = true;
            return literal("true");
        case 'f':
            out->type_ = Type::boolean;
            out->bool_ = false;
            return literal("false");
        case 'n':
            out->type_ = Type::null;
            return literal("null");
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return number(out);
            return fail("unexpected character");
        }
    }

    bool object(Value *out, int depth)
    {
        out->type_ = Type::object;
        ++at_; // '{'
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}')
        {
            ++at_;
            return true;
        }
        for (;;)
        {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"')
                return fail("expected a member name");
            std::string key;
            if (!string(&key))
                return false;
            skip_space();
            if (at_ >= text_.size() || text_[at_] != ':')
                return fail("expected ':'");
            ++at_;
            skip_space();
            out->keys_.push_back(std::move(key));
            out->items_.emplace_back();
            if (!value(&out->items_.back(), depth + 1))
                return false;
            skip_space();
            if (at_ >= text_.size())
                return fail("unexpected end in an object");
            if (text_[at_] == ',')
            {
                ++at_;
                continue;
            }
            if (text_[at_] == '}')
            {
                ++at_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value *out, int depth)
    {
        out->type_ = Type::array;
        ++at_; // '['
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']')
        {
            ++at_;
            return true;
        }
        for (;;)
        {
            skip_space();
            out->items_.emplace_back();
            if (!value(&out->items_.back(), depth + 1))
                return false;
            skip_space();
            if (at_ >= text_.size())
                return fail("unexpected end in an array");
            if (text_[at_] == ',')
            {
                ++at_;
                continue;
            }
            if (text_[at_] == ']')
            {
                ++at_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(std::uint32_t *code)
    {
        if (at_ + 4 > text_.size())
            return fail("short \\u escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = text_[at_++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                return fail("invalid \\u escape");
        }
        *code = value;
        return true;
    }

    bool string(std::string *out)
    {
        ++at_; // opening quote
        for (;;)
        {
            if (at_ >= text_.size())
                return fail("unterminated string");
            const char c = text_[at_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in a string");
            if (c != '\\')
            {
                out->push_back(c);
                continue;
            }
            if (at_ >= text_.size())
                return fail("unterminated escape");
            const char e = text_[at_++];
            switch (e)
            {
            case '"':
            case '\\':
            case '/':
                out->push_back(e);
                break;
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'u':
            {
                std::uint32_t code = 0;
                if (!hex4(&code))
                    return false;
                if (code >= 0xD800 && code <= 0xDBFF)
                {
                    // A high surrogate must be followed by its low half.
                    std::uint32_t low = 0;
                    if (at_ + 2 > text_.size() || text_[at_] != '\\' || text_[at_ + 1] != 'u')
                        return fail("lone surrogate");
                    at_ += 2;
                    if (!hex4(&low) || low < 0xDC00 || low > 0xDFFF)
                        return fail("invalid surrogate pair");
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (code >= 0xDC00 && code <= 0xDFFF)
                {
                    return fail("lone surrogate");
                }
                append_utf8(*out, code);
                break;
            }
            default:
                return fail("invalid escape");
            }
        }
    }

    bool number(Value *out)
    {
        // Strict JSON grammar, evaluated without the C library's locale.
        bool negative = false;
        if (text_[at_] == '-')
        {
            negative = true;
            ++at_;
        }
        if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9')
            return fail("invalid number");
        double mantissa = 0.0;
        if (text_[at_] == '0')
        {
            ++at_;
        }
        else
        {
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
                mantissa = mantissa * 10.0 + (text_[at_++] - '0');
        }
        int exponent = 0;
        if (at_ < text_.size() && text_[at_] == '.')
        {
            ++at_;
            if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9')
                return fail("invalid fraction");
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
            {
                mantissa = mantissa * 10.0 + (text_[at_++] - '0');
                --exponent;
            }
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E'))
        {
            ++at_;
            bool exponent_negative = false;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-'))
                exponent_negative = text_[at_++] == '-';
            if (at_ >= text_.size() || text_[at_] < '0' || text_[at_] > '9')
                return fail("invalid exponent");
            int value = 0;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
            {
                if (value < 100000)
                    value = value * 10 + (text_[at_] - '0');
                ++at_;
            }
            exponent += exponent_negative ? -value : value;
        }
        double result = mantissa;
        if (exponent != 0)
            result = mantissa * std::pow(10.0, static_cast<double>(exponent));
        out->type_ = Type::number;
        out->number_ = negative ? -result : result;
        return true;
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::string error_;
};

bool parse(std::string_view text, Value *out, std::string *error)
{
    Parser parser(text);
    Value result;
    if (!parser.document(&result))
    {
        if (error != nullptr)
            *error = parser.error();
        return false;
    }
    *out = std::move(result);
    return true;
}

std::string quote(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
                out += escape;
            }
            else
            {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace orbit::json
