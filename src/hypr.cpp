#include "hypr.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "util.hpp"

namespace hypr {

static std::string socket_path() {
    std::string sig = util::env("HYPRLAND_INSTANCE_SIGNATURE");
    if (sig.empty()) return "";
    std::string p = util::runtime_dir() + "/hypr/" + sig + "/.socket.sock";
    if (util::exists(p)) return p;
    p = "/tmp/hypr/" + sig + "/.socket.sock";
    return util::exists(p) ? p : "";
}

bool available() { return !socket_path().empty(); }

static bool request(const std::string& msg, std::string& reply) {
    std::string path = socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, path.c_str());
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
              write(fd, msg.data(), msg.size()) == ssize_t(msg.size());
    reply.clear();
    char buf[4096];
    while (ok) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 1000) <= 0) break;   // Hyprland answers fast; never hang
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        reply.append(buf, n);
    }
    close(fd);
    return ok;
}

Json request_json(const std::string& cmd) {
    std::string reply;
    if (!request("j/" + cmd, reply)) return Json();
    return Json::parse(reply);
}

bool dispatch(const std::string& args) {
    std::string reply;
    return request("dispatch " + args, reply) && reply.rfind("ok", 0) == 0;
}

bool focused_monitor(Monitor& out) {
    Json mons = request_json("monitors");
    for (auto& m : mons.arr)
        if (m["focused"].boolean()) {
            out.name = m["name"].string();
            out.width = m["width"].number();
            out.height = m["height"].number();
            out.scale = m["scale"].number(1);
            int transform = int(m["transform"].number());
            if (transform % 2) std::swap(out.width, out.height);
            return true;
        }
    return false;
}

std::vector<std::string> client_addresses() {
    std::vector<std::string> out;
    for (auto& c : request_json("clients").arr) out.push_back(c["address"].string());
    return out;
}

bool cursor_pos(double& x, double& y) {
    Json p = request_json("cursorpos");
    if (p.type != Json::Object) return false;
    x = p["x"].number();
    y = p["y"].number();
    return true;
}

bool capture(const Json& win, const std::string& png) {
    if (!util::have_program("grim")) return false;
    char geom[96];
    std::snprintf(geom, sizeof geom, "%d,%d %dx%d", int(win["at"][0].number()),
                  int(win["at"][1].number()), int(win["size"][0].number()),
                  int(win["size"][1].number()));
    return util::run_wait({"grim", "-s", "0.35", "-g", geom, png}, 3000) == 0;
}

void stash(const std::string& address) {
    dispatch(std::string("movetoworkspacesilent ") + STASH_WORKSPACE + ",address:" + address);
}

void restore(const std::string& address, bool have_cursor, double cx, double cy) {
    Json ws = request_json("activeworkspace");
    if (!ws.has("id")) return;
    int id = int(ws["id"].number());
    dispatch("movetoworkspacesilent " + std::to_string(id) + ",address:" + address);
    if (have_cursor) {
        for (auto& c : request_json("clients").arr) {
            if (c["address"].string() != address || !c["floating"].boolean()) continue;
            int w = int(c["size"][0].number());
            dispatch("movewindowpixel exact " + std::to_string(int(cx - w / 2.0)) + " " +
                     std::to_string(int(cy - 20)) + ",address:" + address);
        }
    }
    dispatch("focuswindow address:" + address);
}

}  // namespace hypr
