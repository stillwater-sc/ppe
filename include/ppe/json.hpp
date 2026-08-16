// json.hpp -- a minimal JSON reader.
//
// PPE has no third-party dependencies and this is not the place to acquire one:
// the whole point of the detection layer is that it builds anywhere with a
// compiler and CMake. What it needs to read is a kpu-sim system configuration
// (configs/systems/*.json), which is machine-generated, well-formed, and small.
//
// SCOPE, stated so nobody mistakes this for a JSON library:
//   * parses objects, arrays, strings, numbers, true/false/null
//   * string escapes: \" \\ \/ \b \f \n \r \t and \uXXXX for the BMP
//   * NOT validated against the spec's edge cases, NOT streaming, NOT fast
//   * a parse failure returns an error with a byte offset, never throws
//
// If PPE ever needs to consume JSON it did not produce or a sibling did not
// produce, replace this rather than extend it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ppe::json {

class value;
using object = std::map<std::string, value>;
using array = std::vector<value>;

enum class kind { null, boolean, number, string, array_t, object_t };

class value {
public:
    value() = default;
    explicit value(bool b) : kind_(kind::boolean), bool_(b) {}
    explicit value(double d) : kind_(kind::number), num_(d) {}
    explicit value(std::string s) : kind_(kind::string), str_(std::move(s)) {}
    explicit value(array a) : kind_(kind::array_t), arr_(std::move(a)) {}
    explicit value(object o) : kind_(kind::object_t), obj_(std::move(o)) {}

    kind type() const { return kind_; }
    bool is_null() const { return kind_ == kind::null; }
    bool is_object() const { return kind_ == kind::object_t; }
    bool is_array() const { return kind_ == kind::array_t; }

    /// Lookups that cannot fail: a missing key or a wrong type yields the
    /// fallback. A configuration reader wants "absent means default", not an
    /// exception per optional field.
    const value& operator[](const std::string& key) const {
        static const value none;
        if (kind_ != kind::object_t) return none;
        const auto it = obj_.find(key);
        return it == obj_.end() ? none : it->second;
    }

    const array& items() const {
        static const array none;
        return kind_ == kind::array_t ? arr_ : none;
    }

    double number(double fallback = 0.0) const {
        return kind_ == kind::number ? num_ : fallback;
    }
    std::size_t size_bytes_from_kb(std::size_t fallback = 0) const {
        return kind_ == kind::number ? static_cast<std::size_t>(num_) * 1024u : fallback;
    }
    std::size_t size_bytes_from_mb(std::size_t fallback = 0) const {
        return kind_ == kind::number ? static_cast<std::size_t>(num_) * 1024u * 1024u
                                     : fallback;
    }
    std::string str(const std::string& fallback = {}) const {
        return kind_ == kind::string ? str_ : fallback;
    }
    bool boolean(bool fallback = false) const {
        return kind_ == kind::boolean ? bool_ : fallback;
    }

private:
    kind kind_ = kind::null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    array arr_;
    object obj_;
};

struct parse_result {
    value root;
    bool ok = false;
    std::string error;
    std::size_t offset = 0;
};

namespace detail {

struct parser {
    const std::string& s;
    std::size_t i = 0;
    std::string error;

    explicit parser(const std::string& text) : s(text) {}

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                                s[i] == '\r')) {
            ++i;
        }
    }

    bool fail(const char* what) {
        if (error.empty()) error = what;
        return false;
    }

    bool parse_value(value& out) {
        skip_ws();
        if (i >= s.size()) return fail("unexpected end of input");
        switch (s[i]) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = value(std::move(str));
                return true;
            }
            case 't':
                if (s.compare(i, 4, "true") != 0) return fail("expected true");
                i += 4;
                out = value(true);
                return true;
            case 'f':
                if (s.compare(i, 5, "false") != 0) return fail("expected false");
                i += 5;
                out = value(false);
                return true;
            case 'n':
                if (s.compare(i, 4, "null") != 0) return fail("expected null");
                i += 4;
                out = value();
                return true;
            default: return parse_number(out);
        }
    }

    bool parse_number(value& out) {
        const std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool any = false;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '-' ||
                                s[i] == '+')) {
            if (s[i] >= '0' && s[i] <= '9') any = true;
            ++i;
        }
        if (!any) return fail("expected a number");
        out = value(std::strtod(s.substr(start, i - start).c_str(), nullptr));
        return true;
    }

    /// Append a code point as UTF-8. \uXXXX is BMP only; a surrogate pair is
    /// encoded as its two halves rather than combined, which is wrong for
    /// astral characters and irrelevant for the configurations this reads --
    /// noted rather than silently assumed.
    static void append_utf8(std::string& out, std::uint32_t cp) {
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
    }

    bool parse_string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return fail("unterminated escape");
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("truncated \\u escape");
                    std::uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                        else return fail("bad hex in \\u escape");
                    }
                    i += 4;
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_array(value& out) {
        ++i;  // '['
        array arr;
        skip_ws();
        if (i < s.size() && s[i] == ']') { ++i; out = value(std::move(arr)); return true; }
        while (true) {
            value v;
            if (!parse_value(v)) return false;
            arr.push_back(std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; out = value(std::move(arr)); return true; }
            return fail("expected , or ] in array");
        }
    }

    bool parse_object(value& out) {
        ++i;  // '{'
        object obj;
        skip_ws();
        if (i < s.size() && s[i] == '}') { ++i; out = value(std::move(obj)); return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (i >= s.size() || s[i] != ':') return fail("expected : after key");
            ++i;
            value v;
            if (!parse_value(v)) return false;
            obj.emplace(std::move(key), std::move(v));
            skip_ws();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; out = value(std::move(obj)); return true; }
            return fail("expected , or } in object");
        }
    }
};

}  // namespace detail

inline parse_result parse(const std::string& text) {
    parse_result r;
    detail::parser p(text);
    if (!p.parse_value(r.root)) {
        r.error = p.error.empty() ? "parse failed" : p.error;
        r.offset = p.i;
        return r;
    }
    p.skip_ws();
    if (p.i != text.size()) {
        r.error = "trailing content after the top-level value";
        r.offset = p.i;
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace ppe::json
