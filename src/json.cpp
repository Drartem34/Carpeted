#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

const Json kNull;

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
    }
    bool lit(const char* w) {
        size_t n = 0;
        while (w[n]) ++n;
        if (s.compare(i, n, w) == 0) { i += n; return true; }
        return false;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }
    std::string str() {
        std::string out;
        ++i;  // opening quote
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                    unsigned lo = std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: out += e;
            }
        }
        if (i >= s.size()) ok = false;
        ++i;
        return out;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (i >= s.size() || depth > 64) { ok = false; return j; }
        char c = s[i];
        if (c == '{') {
            j.type = Json::Object;
            ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; return j; }
            while (ok) {
                ws();
                if (i >= s.size() || s[i] != '"') { ok = false; break; }
                std::string k = str();
                ws();
                if (i >= s.size() || s[i] != ':') { ok = false; break; }
                ++i;
                j.obj[k] = value(depth + 1);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = false;
            }
        } else if (c == '[') {
            j.type = Json::Array;
            ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return j; }
            while (ok) {
                j.arr.push_back(value(depth + 1));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = false;
            }
        } else if (c == '"') {
            j.type = Json::String;
            j.str = str();
        } else if (lit("true")) { j.type = Json::Bool; j.b = true; }
        else if (lit("false")) { j.type = Json::Bool; }
        else if (lit("null")) {}
        else {
            char* end = nullptr;
            j.num = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) { ok = false; return j; }
            j.type = Json::Number;
            i = end - s.c_str();
        }
        return j;
    }
};

void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

void dump(std::string& out, const Json& j) {
    switch (j.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Number: {
        char buf[32];
        if (std::isfinite(j.num) && j.num == std::floor(j.num) && std::fabs(j.num) < 1e15)
            std::snprintf(buf, sizeof buf, "%.0f", j.num);
        else
            std::snprintf(buf, sizeof buf, "%.6g", std::isfinite(j.num) ? j.num : 0.0);
        out += buf;
        break;
    }
    case Json::String: dump_string(out, j.str); break;
    case Json::Array:
        out += '[';
        for (size_t k = 0; k < j.arr.size(); ++k) {
            if (k) out += ',';
            dump(out, j.arr[k]);
        }
        out += ']';
        break;
    case Json::Object: {
        out += '{';
        bool first = true;
        for (auto& [k, v] : j.obj) {
            if (!first) out += ',';
            first = false;
            dump_string(out, k);
            out += ':';
            dump(out, v);
        }
        out += '}';
        break;
    }
    }
}

}  // namespace

Json Json::parse(const std::string& text, bool* ok) {
    Parser p{text};
    Json j = p.value();
    if (ok) *ok = p.ok;
    return p.ok ? j : Json();
}

std::string Json::dump() const {
    std::string out;
    ::dump(out, *this);
    return out;
}

const Json& Json::operator[](const std::string& key) const {
    if (type != Object) return kNull;
    auto it = obj.find(key);
    return it == obj.end() ? kNull : it->second;
}

const Json& Json::operator[](size_t i) const {
    return type == Array && i < arr.size() ? arr[i] : kNull;
}
