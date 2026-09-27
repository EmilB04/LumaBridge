// Minimal JSON reader (RFC 8259 subset: everything GameSense clients send). Header-only,
// no dependencies, depth-limited so hostile input can't blow the stack.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace luma {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    static bool Parse(std::string_view text, Json* out, std::string* error = nullptr) {
        Parser p{text, 0, {}};
        Json v;
        if (!p.Value(&v, 0) || (p.SkipWs(), p.pos != text.size())) {
            if (error) *error = p.error.empty() ? "trailing characters" : p.error;
            return false;
        }
        *out = std::move(v);
        return true;
    }

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    double Number(double fallback = 0) const { return IsNumber() ? num_ : fallback; }
    bool Bool(bool fallback = false) const { return IsBool() ? bool_ : fallback; }
    const std::string& String() const { return str_; }  // empty unless IsString()

    // Arrays and objects. Objects keep insertion order.
    size_t size() const { return children_.size(); }
    const Json& operator[](size_t i) const { return i < children_.size() ? children_[i] : Null(); }
    const Json& operator[](int i) const { return i < 0 ? Null() : (*this)[static_cast<size_t>(i)]; }
    const std::string& KeyAt(size_t i) const { return keys_[i]; }

    // Object member lookup; a shared null value when missing or not an object.
    const Json& operator[](std::string_view key) const {
        if (IsObject())
            for (size_t i = 0; i < keys_.size(); ++i)
                if (keys_[i] == key) return children_[i];
        return Null();
    }
    const Json& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    bool Has(std::string_view key) const { return !(*this)[key].IsNull(); }

    static const Json& Null() {
        static const Json kNull;
        return kNull;
    }

private:
    struct Parser {
        std::string_view s;
        size_t pos;
        std::string error;

        static constexpr int kMaxDepth = 64;

        bool Fail(const char* msg) {
            if (error.empty()) error = std::string(msg) + " at offset " + std::to_string(pos);
            return false;
        }
        void SkipWs() {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
                ++pos;
        }
        bool Literal(std::string_view lit) {
            if (s.substr(pos, lit.size()) != lit) return Fail("invalid literal");
            pos += lit.size();
            return true;
        }

        bool Value(Json* out, int depth) {
            if (depth > kMaxDepth) return Fail("nesting too deep");
            SkipWs();
            if (pos >= s.size()) return Fail("unexpected end");
            char c = s[pos];
            if (c == '{') return Object(out, depth);
            if (c == '[') return Array(out, depth);
            if (c == '"') {
                out->type_ = Type::String;
                return StringLit(&out->str_);
            }
            if (c == 't') { out->type_ = Type::Bool; out->bool_ = true; return Literal("true"); }
            if (c == 'f') { out->type_ = Type::Bool; out->bool_ = false; return Literal("false"); }
            if (c == 'n') { out->type_ = Type::Null; return Literal("null"); }
            if (c == '-' || (c >= '0' && c <= '9')) return NumberLit(out);
            return Fail("unexpected character");
        }

        bool NumberLit(Json* out) {
            size_t start = pos;
            if (s[pos] == '-') ++pos;
            auto digits = [&] {
                size_t b = pos;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
                return pos > b;
            };
            if (!digits()) return Fail("bad number");
            if (pos < s.size() && s[pos] == '.') {
                ++pos;
                if (!digits()) return Fail("bad number");
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                if (!digits()) return Fail("bad number");
            }
            std::string tmp(s.substr(start, pos - start));
            out->type_ = Type::Number;
            out->num_ = std::strtod(tmp.c_str(), nullptr);
            return true;
        }

        static void AppendUtf8(std::string* o, unsigned cp) {
            if (cp < 0x80) {
                *o += static_cast<char>(cp);
            } else if (cp < 0x800) {
                *o += static_cast<char>(0xC0 | (cp >> 6));
                *o += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                *o += static_cast<char>(0xE0 | (cp >> 12));
                *o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *o += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                *o += static_cast<char>(0xF0 | (cp >> 18));
                *o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                *o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                *o += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        bool Hex4(unsigned* out) {
            if (pos + 4 > s.size()) return Fail("bad \\u escape");
            unsigned v = 0;
            for (int i = 0; i < 4; ++i) {
                char h = s[pos++];
                v <<= 4;
                if (h >= '0' && h <= '9') v |= h - '0';
                else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                else return Fail("bad \\u escape");
            }
            *out = v;
            return true;
        }

        bool StringLit(std::string* out) {
            ++pos;  // opening quote
            while (pos < s.size()) {
                char c = s[pos++];
                if (c == '"') return true;
                if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
                if (c != '\\') {
                    *out += c;
                    continue;
                }
                if (pos >= s.size()) break;
                char e = s[pos++];
                switch (e) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s.substr(pos, 2) == "\\u") {
                        pos += 2;
                        unsigned lo = 0;
                        if (!Hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("bad escape");
                }
            }
            return Fail("unterminated string");
        }

        bool Array(Json* out, int depth) {
            ++pos;
            out->type_ = Type::Array;
            SkipWs();
            if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
            while (true) {
                Json v;
                if (!Value(&v, depth + 1)) return false;
                out->children_.push_back(std::move(v));
                SkipWs();
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
                return Fail("expected , or ]");
            }
        }

        bool Object(Json* out, int depth) {
            ++pos;
            out->type_ = Type::Object;
            SkipWs();
            if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
            while (true) {
                SkipWs();
                if (pos >= s.size() || s[pos] != '"') return Fail("expected key");
                std::string key;
                if (!StringLit(&key)) return false;
                SkipWs();
                if (pos >= s.size() || s[pos] != ':') return Fail("expected :");
                ++pos;
                Json v;
                if (!Value(&v, depth + 1)) return false;
                out->keys_.push_back(std::move(key));
                out->children_.push_back(std::move(v));
                SkipWs();
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
                return Fail("expected , or }");
            }
        }
    };

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<std::string> keys_;  // objects only, parallel to children_
    std::vector<Json> children_;
};

// Escapes a string for embedding in a JSON response.
inline std::string JsonEscape(std::string_view s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                o += buf;
            } else {
                o += c;
            }
        }
    }
    return o;
}

}  // namespace luma
