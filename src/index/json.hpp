#pragma once

// Bounded JSON reader/writer for FORMAT-1 artifacts (CURRENT, manifest,
// segment.json, build configuration). Deliberately small: strict syntax,
// duplicate keys rejected, explicit depth/size limits, no locale dependence.
// FORMAT-1 forbids emitting anything but compact UTF-8 without BOM or newline.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agent_io::index_json {

inline constexpr std::size_t kMaxDepth = 64;
inline constexpr std::size_t kMaxElements = 1u << 20;
inline constexpr std::size_t kMaxStringBytes = 1u << 20;

class Value {
public:
    enum class Kind { null, boolean, integer, real, string, array, object };

    Value() = default;

    static Value Null();
    static Value Boolean(bool value);
    static Value Unsigned(std::uint64_t value);
    static Value Signed(std::int64_t value);
    static Value Real(double value);
    static Value String(std::string value);
    static Value Array();
    static Value Object();

    Kind GetKind() const noexcept { return kind_; }
    bool IsNull() const noexcept { return kind_ == Kind::null; }
    bool IsBoolean() const noexcept { return kind_ == Kind::boolean; }
    bool IsInteger() const noexcept { return kind_ == Kind::integer; }
    bool IsReal() const noexcept { return kind_ == Kind::real; }
    bool IsString() const noexcept { return kind_ == Kind::string; }
    bool IsArray() const noexcept { return kind_ == Kind::array; }
    bool IsObject() const noexcept { return kind_ == Kind::object; }

    // Precondition checks are the caller's job; the accessors never throw.
    bool AsBool() const noexcept { return boolean_; }
    bool Negative() const noexcept { return negative_; }
    std::uint64_t Magnitude() const noexcept { return magnitude_; }
    double AsReal() const noexcept { return real_; }
    const std::string& AsString() const noexcept { return text_; }

    std::size_t Size() const noexcept {
        return kind_ == Kind::object ? members_.size() : items_.size();
    }
    std::size_t MemberCount() const noexcept { return members_.size(); }
    const std::pair<std::string, Value>& Member(std::size_t index) const noexcept {
        return members_[index];
    }
    const Value& At(std::size_t index) const noexcept { return items_[index]; }
    // Linear scan; index artifacts have a handful of keys by construction.
    const Value* Find(std::string_view key) const noexcept;

    void Push(Value value);
    void Insert(std::string key, Value value);

private:
    Kind kind_ = Kind::null;
    bool boolean_ = false;
    bool negative_ = false;
    std::uint64_t magnitude_ = 0;
    double real_ = 0.0;
    std::string text_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error;
};

ParseResult Parse(std::string_view text);

// Compact UTF-8, no BOM, no trailing newline. Invalid input yields an empty
// string; callers that need the exact byte count use SerializedSize first.
std::string Write(const Value& value);
bool WriteTo(const Value& value, std::string& output);

// Escaping used by both writers above so the two paths cannot diverge.
void AppendEscaped(const std::string& text, std::string& output);
bool IsValidUtf8(std::string_view text);

}  // namespace agent_io::index_json
