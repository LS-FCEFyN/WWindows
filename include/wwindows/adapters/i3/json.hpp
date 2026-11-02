#pragma once

// A deliberately small JSON reader, just enough for i3's IPC replies and
// events. Not a general-purpose library: no serialization, no streaming, no
// number formatting. It exists because pulling a JSON dependency into a
// compositor for two message shapes is a worse trade than ~150 lines that
// are exercised by the i3 test suite.
//
// Defensive where it matters: the input comes from a local socket we trust
// about as far as any other IPC peer, so nesting depth is capped (a hostile
// or corrupted `[[[[...` must not overflow the stack) and every failure is a
// clean std::nullopt rather than an exception or a partial value.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ww::json {

struct Value {
  enum class Kind { Null, Bool, Number, String, Array, Object };

  Kind kind{Kind::Null};
  bool boolean{false};
  double number{0.0};
  std::string string;
  // Array elements, or object member values (parallel to `keys`).
  std::vector<Value> items;
  std::vector<std::string> keys;

  // Object member lookup; nullptr for a missing key or a non-object.
  [[nodiscard]] const Value *find(std::string_view key) const noexcept {
    if (kind != Kind::Object)
      return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i)
      if (keys[i] == key)
        return &items[i];
    return nullptr;
  }
};

namespace detail {

constexpr int kMaxDepth = 128;

class Parser {
public:
  explicit Parser(std::string_view text) : s_{text} {}

  std::optional<Value> parse_document() {
    Value v;
    if (!parse_value(v, 0))
      return std::nullopt;
    skip_ws();
    if (i_ != s_.size())
      return std::nullopt; // trailing garbage
    return v;
  }

private:
  std::string_view s_;
  std::size_t i_{0};

  void skip_ws() {
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
      ++i_;
  }

  bool consume(char c) {
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }

  bool literal(std::string_view word) {
    if (s_.substr(i_, word.size()) != word)
      return false;
    i_ += word.size();
    return true;
  }

  static void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t &out) {
    if (i_ + 4 > s_.size())
      return false;
    out = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_++];
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= static_cast<uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }

  bool parse_string(std::string &out) {
    if (!consume('"'))
      return false;
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return false; // raw control characters are not legal in a string
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i_ >= s_.size())
        return false;
      switch (const char e = s_[i_++]; e) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        uint32_t cp = 0;
        if (!hex4(cp))
          return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          // High surrogate: only meaningful with a following low one.
          uint32_t lo = 0;
          if (s_.substr(i_, 2) == "\\u") {
            i_ += 2;
            if (!hex4(lo))
              return false;
          }
          cp = (lo >= 0xDC00 && lo <= 0xDFFF)
                   ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                   : 0xFFFD;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        append_utf8(out, cp);
        break;
      }
      default:
        return false;
      }
    }
    return false; // unterminated
  }

  bool parse_number(double &out) {
    const std::size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-')
      ++i_;
    while (i_ < s_.size() &&
           ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
            s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
      ++i_;
    if (i_ == start)
      return false;
    const std::string text{s_.substr(start, i_ - start)};
    char *end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
  }

  bool parse_value(Value &v, int depth) {
    if (depth > kMaxDepth)
      return false;
    skip_ws();
    if (i_ >= s_.size())
      return false;

    switch (s_[i_]) {
    case '{': {
      ++i_;
      v.kind = Value::Kind::Object;
      skip_ws();
      if (consume('}'))
        return true;
      for (;;) {
        skip_ws();
        std::string key;
        if (!parse_string(key))
          return false;
        skip_ws();
        if (!consume(':'))
          return false;
        Value member;
        if (!parse_value(member, depth + 1))
          return false;
        v.keys.push_back(std::move(key));
        v.items.push_back(std::move(member));
        skip_ws();
        if (consume(','))
          continue;
        return consume('}');
      }
    }
    case '[': {
      ++i_;
      v.kind = Value::Kind::Array;
      skip_ws();
      if (consume(']'))
        return true;
      for (;;) {
        Value element;
        if (!parse_value(element, depth + 1))
          return false;
        v.items.push_back(std::move(element));
        skip_ws();
        if (consume(','))
          continue;
        return consume(']');
      }
    }
    case '"':
      v.kind = Value::Kind::String;
      return parse_string(v.string);
    case 't':
      v.kind = Value::Kind::Bool;
      v.boolean = true;
      return literal("true");
    case 'f':
      v.kind = Value::Kind::Bool;
      v.boolean = false;
      return literal("false");
    case 'n':
      v.kind = Value::Kind::Null;
      return literal("null");
    default:
      v.kind = Value::Kind::Number;
      return parse_number(v.number);
    }
  }
};

} // namespace detail

// Parses one complete JSON document; std::nullopt if it is not valid JSON.
[[nodiscard]] inline std::optional<Value> parse(std::string_view text) {
  return detail::Parser{text}.parse_document();
}

} // namespace ww::json
