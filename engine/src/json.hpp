// Minimal JSON reader. Enough for scores and voice files; no dependencies.
#pragma once

#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace novox::json {

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<Value> array;
  std::map<std::string, Value> object;

  bool isNull() const { return type == Type::Null; }
  bool isNumber() const { return type == Type::Number; }
  bool isString() const { return type == Type::String; }
  bool isArray() const { return type == Type::Array; }
  bool isObject() const { return type == Type::Object; }

  bool has(const std::string& key) const {
    return type == Type::Object && object.count(key) > 0;
  }
  const Value& operator[](const std::string& key) const {
    static const Value null;
    if (type != Type::Object) return null;
    auto it = object.find(key);
    return it == object.end() ? null : it->second;
  }
  double num(const std::string& key, double fallback) const {
    const Value& v = (*this)[key];
    return v.isNumber() ? v.number : fallback;
  }
  std::string str(const std::string& key, const std::string& fallback) const {
    const Value& v = (*this)[key];
    return v.isString() ? v.string : fallback;
  }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  Value parse() {
    Value v = value();
    ws();
    if (i_ != s_.size()) fail("trailing characters");
    return v;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  [[noreturn]] void fail(const std::string& msg) {
    throw std::runtime_error("JSON error at offset " + std::to_string(i_) + ": " + msg);
  }

  void ws() {
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i_;
      } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
        // Allow // comments so hand-written scores can be annotated.
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
      } else {
        break;
      }
    }
  }

  bool consume(const char* lit) {
    size_t n = std::char_traits<char>::length(lit);
    if (s_.compare(i_, n, lit) == 0) {
      i_ += n;
      return true;
    }
    return false;
  }

  Value value() {
    ws();
    if (i_ >= s_.size()) fail("unexpected end");
    char c = s_[i_];
    Value v;
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') {
      v.type = Value::Type::String;
      v.string = string();
      return v;
    }
    if (consume("true")) {
      v.type = Value::Type::Bool;
      v.boolean = true;
      return v;
    }
    if (consume("false")) {
      v.type = Value::Type::Bool;
      return v;
    }
    if (consume("null")) return v;
    return number();
  }

  Value number() {
    const char* start = s_.c_str() + i_;
    char* end = nullptr;
    double d = std::strtod(start, &end);
    if (end == start) fail("unexpected character");
    i_ += static_cast<size_t>(end - start);
    Value v;
    v.type = Value::Type::Number;
    v.number = d;
    return v;
  }

  std::string string() {
    ++i_;  // opening quote
    std::string out;
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) fail("bad escape");
      char e = s_[i_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (i_ + 4 > s_.size()) fail("bad unicode escape");
          unsigned cp = std::stoul(s_.substr(i_, 4), nullptr, 16);
          i_ += 4;
          // Encode as UTF-8 (BMP only; lyrics rarely need more).
          if (cp < 0x80) {
            out += static_cast<char>(cp);
          } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          }
          break;
        }
        default: out += e;
      }
    }
    if (i_ >= s_.size()) fail("unterminated string");
    ++i_;  // closing quote
    return out;
  }

  Value array() {
    ++i_;
    Value v;
    v.type = Value::Type::Array;
    ws();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return v;
    }
    while (true) {
      v.array.push_back(value());
      ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == ']') {  // tolerate trailing comma
          ++i_;
          return v;
        }
        continue;
      }
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return v;
      }
      fail("expected , or ]");
    }
  }

  Value object() {
    ++i_;
    Value v;
    v.type = Value::Type::Object;
    ws();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return v;
    }
    while (true) {
      ws();
      if (i_ >= s_.size() || s_[i_] != '"') fail("expected key");
      std::string key = string();
      ws();
      if (i_ >= s_.size() || s_[i_] != ':') fail("expected :");
      ++i_;
      v.object[key] = value();
      ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
          ++i_;
          return v;
        }
        continue;
      }
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return v;
      }
      fail("expected , or }");
    }
  }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

}  // namespace novox::json
