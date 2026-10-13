// carpeted - a rug with real cloth physics that rolls out from the bottom
// of the screen.  Things lie next to it; lift it and hide things under it.
//
//   carpeted               start the background daemon (rug rolled up)
//   carpeted toggle        roll the rug out / back in (bind it to SUPER+R)
//   carpeted stash-window  put the active window on the rug
//   carpeted show|hide|shake|quit
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.hpp"
#include "daemon.hpp"
#include "util.hpp"

namespace {

std::string socket_path() {
    std::string disp = util::env("WAYLAND_DISPLAY", "wayland-0");
    for (auto& ch : disp)
        if (ch == '/') ch = '_';
    return util::runtime_dir() + "/carpeted-" + disp + ".sock";
}

bool fill_addr(sockaddr_un& addr, const std::string& path) {
    if (path.size() >= sizeof addr.sun_path) return false;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, path.c_str());
    return true;
}

// Send a command to a running daemon; false if there is none.
bool send_command(const std::string& cmd) {
    sockaddr_un addr;
    if (!fill_addr(addr, socket_path())) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return false;
    }
    std::string line = cmd + "\n";
    bool ok = write(fd, line.data(), line.size()) == ssize_t(line.size());
    char buf[16];
    if (ok) (void)!read(fd, buf, sizeof buf);    // wait until it was handled
    close(fd);
    return ok;
}

int listen_socket() {
    sockaddr_un addr;
    std::string path = socket_path();
    if (!fill_addr(addr, path)) return -1;
    unlink(path.c_str());                 // stale socket of a dead daemon
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void usage() {
    std::puts(
        "usage: carpeted [command]\n"
        "\n"
        "  (none)        run the daemon; the rug starts rolled up\n"
        "  toggle        roll the rug out or back in      (bind to SUPER+R)\n"
        "  show | hide   roll it out / in\n"
        "  stash-window  put the active window on the rug (bind to SUPER+SHIFT+R)\n"
        "  shake         shake the rug out flat\n"
        "  quit          stop the daemon\n"
        "\n"
        "Config: ~/.config/carpeted/config   State: ~/.local/state/carpeted/");
}

volatile sig_atomic_t g_stop = 0;

}  // namespace

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }

    struct sigaction sa {};
    sa.sa_handler = [](int) { g_stop = 1; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    Config cfg = Config::load();
    cfg.debug = util::env("CARPETED_DEBUG") == "1";

    // The rug process, started by the daemon: lives while the rug is out.
    if (cmd == "--ui" && argc > 2) {
        App app(cfg);
        int rc = app.run(std::atoi(argv[2]), &g_stop);
        return rc;
    }

    if (cmd == "daemon") cmd.clear();
    static const char* known[] = {"", "toggle", "show", "hide", "stash-window", "shake", "quit"};
    bool ok = cmd.rfind("debug-", 0) == 0;
    for (auto k : known) ok = ok || cmd == k;
    if (!ok) {
        usage();
        return 2;
    }
    std::string full = cmd;
    for (int k = 2; k < argc; ++k) full += std::string(" ") + argv[k];

    // A daemon is already running: just hand it the command.
    if (!cmd.empty() && send_command(full)) return 0;
    if (cmd.empty() && send_command("ping")) {
        std::fprintf(stderr, "carpeted: already running\n");
        return 0;
    }
    if (cmd == "quit" || cmd == "hide" || cmd == "shake") return 0;

    // Become the daemon.  "toggle"/"show"/"stash-window" without one running
    // start it and run the command in it.
    int lfd = listen_socket();
    if (lfd < 0) {
        std::fprintf(stderr, "carpeted: cannot create %s\n", socket_path().c_str());
        return 1;
    }
    int rc;
    {
        Daemon daemon(cfg);
        rc = daemon.run(lfd, full, &g_stop);
    }
    close(lfd);
    unlink(socket_path().c_str());
    return rc;
}
