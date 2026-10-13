// Minimal JSON: enough for our state file and Hyprland's IPC replies.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    static Json parse(const std::string& text, bool* ok = nullptr);
    std::string dump() const;

    const Json& operator[](const std::string& key) const;
    const Json& operator[](size_t i) const;
    bool has(const std::string& key) const { return type == Object && obj.count(key); }
    double number(double def = 0) const { return type == Number ? num : def; }
    std::string string(const std::string& def = "") const {
        return type == String ? str : def;
    }
    bool boolean(bool def = false) const { return type == Bool ? b : def; }

    static Json of(double v) { Json j; j.type = Number; j.num = v; return j; }
    static Json of(const std::string& v) { Json j; j.type = String; j.str = v; return j; }
    static Json of(bool v) { Json j; j.type = Bool; j.b = v; return j; }
    static Json object() { Json j; j.type = Object; return j; }
    static Json array() { Json j; j.type = Array; return j; }
};
