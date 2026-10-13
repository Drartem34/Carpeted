// Hyprland IPC over its UNIX socket (no hyprctl processes).
#pragma once
#include <string>
#include <vector>

#include "json.hpp"

namespace hypr {

constexpr const char* STASH_WORKSPACE = "special:carpeted";

bool available();
Json request_json(const std::string& cmd);       // e.g. "activewindow"
bool dispatch(const std::string& args);

struct Monitor {
    std::string name;
    double width = 0, height = 0, scale = 1;     // physical px
};
bool focused_monitor(Monitor& out);
std::vector<std::string> client_addresses();
bool cursor_pos(double& x, double& y);

// Window thumbnail through grim (if installed).
bool capture(const Json& window, const std::string& png_path);
void stash(const std::string& address);
void restore(const std::string& address, bool have_cursor, double cx, double cy);

}  // namespace hypr
