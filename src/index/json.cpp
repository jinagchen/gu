#include "json.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace agent_io::index_json {
namespace {

constexpr std::uint32_t kMaxUint32 = 0xffffffffu;

void AppendCodepoint(std::uint32_t code, std::string& output) {
    if (code <= 0x7f) {
        output.push_back(static_cast<char>(code));
    } else if (code <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (code >> 6)));
        output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (code >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult Run() {
        ParseResult result;
        SkipWhitespace();
        if (!ParseValue(0)) {
            result.error = error_.empty() ? "Invalid JSON" : error_;
            return result;
        }
        SkipWhitespace();
        if (position_ != text_.size()) {
            result.error = "Trailing content after JSON value";
            return result;
        }
        result.ok = true;
        result.value = std::move(value_);
        return result;
    }

private:
    bool Fail(const char* message) {
        if (error_.empty()) {
            error_ = message;
            error_ += " at offset ";
            error_ += std::to_string(position_);
        }
        return false;
    }

    bool AtEnd() const { return position_ >= text_.size(); }
    char Peek() const { return text_[position_]; }

    void SkipWhitespace() {
        while (!AtEnd()) {
            const char current = Peek();
            if (current == ' ' || current == '\t' || current == '\n' || current == '\r')
                ++position_;
            else
                break;
        }
    }

    bool Literal(std::string_view text) {
        if (text_.size() - position_ < text.size()) return Fail("Truncated literal");
        if (text_.compare(position_, text.size(), text) != 0) return Fail("Invalid literal");
        position_ += text.size();
        return true;
    }

    static bool IsDigit(char value) { return value >= '0' && value <= '9'; }

    bool ParseHex4(std::uint32_t& value) {
        if (text_.size() - position_ < 4) return Fail("Truncated \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char digit = text_[position_ + static_cast<std::size_t>(i)];
            std::uint32_t part = 0;
            if (digit >= '0' && digit <= '9') part = static_cast<std::uint32_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') part = static_cast<std::uint32_t>(digit - 'a') + 10;
            else if (digit >= 'A' && digit <= 'F') part = static_cast<std::uint32_t>(digit - 'A') + 10;
            else return Fail("Invalid \\u escape");
            value = value * 16 + part;
        }
        position_ += 4;
        return true;
    }

    bool ParseString(std::string& output) {
        if (AtEnd() || Peek() != '"') return Fail("Expected string");
        ++position_;
        output.clear();
        while (true) {
            if (AtEnd()) return Fail("Unterminated string");
            const unsigned char raw = static_cast<unsigned char>(text_[position_]);
            if (raw == '"') { ++position_; break; }
            if (raw < 0x20) return Fail("Control character in string");
            if (raw != '\\') {
                output.push_back(static_cast<char>(raw));
                ++position_;
                continue;
            }
            ++position_;
            if (AtEnd()) return Fail("Unterminated escape");
            const char escape = text_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = 0;
                if (!ParseHex4(code)) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    std::uint32_t low = 0;
                    if (text_.size() - position_ < 2 || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u')
                        return Fail("Missing low surrogate");
                    position_ += 2;
                    if (!ParseHex4(low)) return false;
                    if (low < 0xdc00 || low > 0xdfff) return Fail("Invalid low surrogate");
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) {
                    return Fail("Lone low surrogate");
                }
                AppendCodepoint(code, output);
                break;
            }
            default: return Fail("Unknown escape sequence");
            }
            if (output.size() > kMaxStringBytes) return Fail("String exceeds limit");
        }
        if (output.size() > kMaxStringBytes) return Fail("String exceeds limit");
        if (!IsValidUtf8(output)) return Fail("Invalid UTF-8 in string");
        return true;
    }

    bool ParseNumber(Value& value) {
        const std::size_t start = position_;
        bool negative = false;
        if (!AtEnd() && Peek() == '-') { negative = true; ++position_; }
        if (AtEnd() || !IsDigit(Peek())) return Fail("Invalid number");
        if (Peek() == '0') {
            ++position_;
            if (!AtEnd() && IsDigit(Peek())) return Fail("Leading zero in number");
        } else {
            while (!AtEnd() && IsDigit(Peek())) ++position_;
        }
        bool integral = true;
        if (!AtEnd() && Peek() == '.') {
            integral = false;
            ++position_;
            if (AtEnd() || !IsDigit(Peek())) return Fail("Missing fraction digits");
            while (!AtEnd() && IsDigit(Peek())) ++position_;
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            integral = false;
            ++position_;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) ++position_;
            if (AtEnd() || !IsDigit(Peek())) return Fail("Missing exponent digits");
            while (!AtEnd() && IsDigit(Peek())) ++position_;
        }
        const std::string_view literal = text_.substr(start, position_ - start);
        if (!integral) {
            const std::string copy(literal);
            const double parsed = std::strtod(copy.c_str(), nullptr);
            if (!std::isfinite(parsed)) return Fail("Nonfinite number");
            value = Value::Real(parsed);
            return true;
        }
        std::uint64_t magnitude = 0;
        for (std::size_t index = negative ? 1u : 0u; index < literal.size(); ++index) {
            const auto digit = static_cast<std::uint64_t>(literal[index] - '0');
            if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
                return Fail("Integer out of range");
            magnitude = magnitude * 10 + digit;
        }
        if (negative) {
            if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return Fail("Integer out of range");
            value = Value::Signed(-static_cast<std::int64_t>(magnitude));
        } else {
            value = Value::Unsigned(magnitude);
        }
        return true;
    }

    bool ParseValue(std::size_t depth) {
        if (depth > kMaxDepth) return Fail("Maximum nesting depth exceeded");
        if (AtEnd()) return Fail("Unexpected end of input");
        const char current = Peek();
        Value parsed;
        switch (current) {
        case '{': {
            if (!ParseObject(depth, parsed)) return false;
            break;
        }
        case '[': {
            if (!ParseArray(depth, parsed)) return false;
            break;
        }
        case '"': {
            std::string text;
            if (!ParseString(text)) return false;
            parsed = Value::String(std::move(text));
            break;
        }
        case 't': {
            if (!Literal("true")) return false;
            parsed = Value::Boolean(true);
            break;
        }
        case 'f': {
            if (!Literal("false")) return false;
            parsed = Value::Boolean(false);
            break;
        }
        case 'n': {
            if (!Literal("null")) return false;
            parsed = Value::Null();
            break;
        }
        default: {
            if (!ParseNumber(parsed)) return false;
            break;
        }
        }
        value_ = std::move(parsed);
        return true;
    }

    bool ParseObject(std::size_t depth, Value& output) {
        ++position_;  // '{'
        output = Value::Object();
        SkipWhitespace();
        if (!AtEnd() && Peek() == '}') { ++position_; return true; }
        while (true) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) return false;
            if (output.Find(key) != nullptr) return Fail("Duplicate object key");
            SkipWhitespace();
            if (AtEnd() || Peek() != ':') return Fail("Expected ':'");
            ++position_;
            SkipWhitespace();
            if (!ParseValue(depth + 1)) return false;
            output.Insert(std::move(key), std::move(value_));
            if (output.Size() > kMaxElements) return Fail("Too many object members");
            SkipWhitespace();
            if (AtEnd()) return Fail("Unterminated object");
            if (Peek() == ',') { ++position_; continue; }
            if (Peek() == '}') { ++position_; return true; }
            return Fail("Expected ',' or '}'");
        }
    }

    bool ParseArray(std::size_t depth, Value& output) {
        ++position_;  // '['
        output = Value::Array();
        SkipWhitespace();
        if (!AtEnd() && Peek() == ']') { ++position_; return true; }
        while (true) {
            SkipWhitespace();
            if (!ParseValue(depth + 1)) return false;
            output.Push(std::move(value_));
            if (output.Size() > kMaxElements) return Fail("Too many array elements");
            SkipWhitespace();
            if (AtEnd()) return Fail("Unterminated array");
            if (Peek() == ',') { ++position_; continue; }
            if (Peek() == ']') { ++position_; return true; }
            return Fail("Expected ',' or ']'");
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::string error_;
    Value value_;
};

}  // namespace

Value Value::Null() { return Value{}; }

Value Value::Boolean(bool value) {
    Value result;
    result.kind_ = Kind::boolean;
    result.boolean_ = value;
    return result;
}

Value Value::Unsigned(std::uint64_t value) {
    Value result;
    result.kind_ = Kind::integer;
    result.magnitude_ = value;
    return result;
}

Value Value::Signed(std::int64_t value) {
    Value result;
    result.kind_ = Kind::integer;
    if (value < 0) {
        result.negative_ = true;
        result.magnitude_ = static_cast<std::uint64_t>(-(value + 1)) + 1;
    } else {
        result.magnitude_ = static_cast<std::uint64_t>(value);
    }
    return result;
}

Value Value::Real(double value) {
    Value result;
    result.kind_ = Kind::real;
    result.real_ = value;
    return result;
}

Value Value::String(std::string value) {
    Value result;
    result.kind_ = Kind::string;
    result.text_ = std::move(value);
    return result;
}

Value Value::Array() {
    Value result;
    result.kind_ = Kind::array;
    return result;
}

Value Value::Object() {
    Value result;
    result.kind_ = Kind::object;
    return result;
}

const Value* Value::Find(std::string_view key) const noexcept {
    for (const auto& member : members_)
        if (member.first == key) return &member.second;
    return nullptr;
}

void Value::Push(Value value) {
    if (kind_ == Kind::array) items_.push_back(std::move(value));
}

void Value::Insert(std::string key, Value value) {
    if (kind_ == Kind::object) members_.emplace_back(std::move(key), std::move(value));
}

ParseResult Parse(std::string_view text) { return Parser(text).Run(); }

void AppendEscaped(const std::string& text, std::string& output) {
    static const char* kHex = "0123456789abcdef";
    for (const char item : text) {
        const unsigned char raw = static_cast<unsigned char>(item);
        switch (raw) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (raw < 0x20) {
                output += "\\u00";
                output.push_back(kHex[raw >> 4]);
                output.push_back(kHex[raw & 0x0f]);
            } else {
                output.push_back(item);
            }
            break;
        }
    }
}

namespace {

void AppendInteger(const Value& value, std::string& output) {
    if (value.Negative()) {
        output.push_back('-');
        output += std::to_string(value.Magnitude());
    } else {
        output += std::to_string(value.Magnitude());
    }
}

void AppendReal(double value, std::string& output) {
    char buffer[64]{};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        output += "0";
        return;
    }
    std::string text(buffer, static_cast<std::size_t>(written));
    // Keep the literal recognisably a real number; consumers reject integers
    // where a real is required only when the kind differs, so preserve '.', 'e'.
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    output += text;
}

bool Append(const Value& value, std::string& output) {
    switch (value.GetKind()) {
    case Value::Kind::null: output += "null"; return true;
    case Value::Kind::boolean: output += value.AsBool() ? "true" : "false"; return true;
    case Value::Kind::integer: AppendInteger(value, output); return true;
    case Value::Kind::real: AppendReal(value.AsReal(), output); return true;
    case Value::Kind::string:
        output.push_back('"');
        AppendEscaped(value.AsString(), output);
        output.push_back('"');
        return true;
    case Value::Kind::array: {
        output.push_back('[');
        for (std::size_t index = 0; index < value.Size(); ++index) {
            if (index) output.push_back(',');
            if (!Append(value.At(index), output)) return false;
        }
        output.push_back(']');
        return true;
    }
    case Value::Kind::object: {
        output.push_back('{');
        for (std::size_t index = 0; index < value.MemberCount(); ++index) {
            if (index) output.push_back(',');
            output.push_back('"');
            AppendEscaped(value.Member(index).first, output);
            output += "\":";
            if (!Append(value.Member(index).second, output)) return false;
        }
        output.push_back('}');
        return true;
    }
    }
    return false;
}

}  // namespace

std::string Write(const Value& value) {
    std::string output;
    if (!Append(value, output)) return {};
    return output;
}

bool WriteTo(const Value& value, std::string& output) {
    output.clear();
    return Append(value, output);
}

bool IsValidUtf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (first <= 0x7f) { ++index; continue; }
        if ((first & 0xe0) == 0xc0) { length = 2; code = first & 0x1fu; }
        else if ((first & 0xf0) == 0xe0) { length = 3; code = first & 0x0fu; }
        else if ((first & 0xf8) == 0xf0) { length = 4; code = first & 0x07u; }
        else return false;
        if (text.size() - index < length) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3fu);
        }
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
            (length == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            return false;
        index += length;
    }
    return true;
}

}  // namespace agent_io::index_json
