#include "daemon.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <sstream>
#include <vector>

#include "hypr.hpp"
#include "util.hpp"

Daemon::Daemon(const Config& cfg) : cfg_(cfg), shelf_(cfg.shelf) { shelf_.load(); }

int Daemon::run(int listen_fd, const std::string& first, const volatile sig_atomic_t* stop) {
    if (!first.empty()) handle(first);
    std::vector<int> clients;
    while (!quit_ && !*stop) {
        std::vector<pollfd> fds = {{listen_fd, POLLIN, 0}, {shelf_.watch_fd(), POLLIN, 0},
                                   {child_fd_, POLLIN, 0}};
        for (int c : clients) fds.push_back({c, POLLIN, 0});
        if (poll(fds.data(), fds.size(), -1) <= 0) continue;   // sleeps

        if (fds[0].revents & POLLIN) {
            int c = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (c >= 0) clients.push_back(c);
        }
        if (fds[1].revents & POLLIN) {
            if (child_ > 0) {
                shelf_.drain_watch();             // the rug process watches itself
            } else if (shelf_.handle_watch([this](const Item& it) {
                           const Rect& r = shelf_.rug;     // it comes back flat
                           return !it.on_rug && it.x > r.x0 && it.x < r.x1 && it.y > r.y0 && it.y < r.y1;
                       })) {
                shelf_.save();
            }
        }
        if (child_fd_ >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[64];
            if (read(child_fd_, buf, sizeof buf) <= 0) child_gone();
        }
        std::vector<int> still;
        for (size_t k = 0; k < clients.size(); ++k) {
            int c = clients[k];
            if (!(fds[3 + k].revents & (POLLIN | POLLHUP))) {
                still.push_back(c);
                continue;
            }
            char buf[2048];
            ssize_t len = read(c, buf, sizeof buf - 1);
            if (len > 0) {
                buf[len] = 0;
                std::istringstream in(buf);
                std::string line;
                while (std::getline(in, line))
                    if (!util::trim(line).empty()) handle(util::trim(line));
                (void)!write(c, "ok\n", 3);
            }
            close(c);
        }
        clients = still;
    }
    for (int c : clients) close(c);
    if (child_ > 0) {
        forward("quit");
        int64_t end = util::now_ms() + 3000;
        while (child_ > 0 && util::now_ms() < end) {
            if (waitpid(child_, nullptr, WNOHANG) == child_) break;
            usleep(20000);
        }
        kill(child_, SIGTERM);
        waitpid(child_, nullptr, 0);
    }
    return 0;
}

void Daemon::handle(const std::string& line) {
    std::string cmd = line.substr(0, line.find(' '));
    if (cmd == "ping") return;
    if (child_ > 0) {
        if (forward(line)) {
            if (cmd == "quit") quit_ = true;
            return;
        }
        child_gone();                 // it just exited; handle the command here
    }
    if (cmd == "toggle" || cmd == "show") {
        spawn_ui();
    } else if (cmd == "stash-window") {
        stash_window();
    } else if (cmd == "quit") {
        quit_ = true;
    }
    // hide / shake / debug-*: nothing is out, nothing to do
}

bool Daemon::forward(const std::string& cmd) {
    std::string l = cmd + "\n";
    return write(child_fd_, l.data(), l.size()) == ssize_t(l.size());
}

void Daemon::spawn_ui() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return;
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        close(sv[0]);
        close(sv[1]);
        return;
    }
    exe[n] = 0;
    pid_t pid = fork();
    if (pid == 0) {
        fcntl(sv[1], F_SETFD, 0);                 // keep this end across exec
        std::string fd = std::to_string(sv[1]);
        execl(exe, exe, "--ui", fd.c_str(), nullptr);
        _exit(127);
    }
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        return;
    }
    child_ = pid;
    child_fd_ = sv[0];
    forward("show");
}

void Daemon::child_gone() {
    if (child_fd_ >= 0) close(child_fd_);
    child_fd_ = -1;
    if (child_ > 0) waitpid(child_, nullptr, 0);
    child_ = -1;
    shelf_.load();            // the rug process saved what changed
}

void Daemon::stash_window() {
    if (!hypr::available()) {
        std::fprintf(stderr, "carpeted: window stashing needs Hyprland\n");
        return;
    }
    Json win = hypr::request_json("activewindow");
    std::string addr = win["address"].string();
    if (addr.empty() || win["workspace"]["name"].string() == hypr::STASH_WORKSPACE) return;
    std::string dir = util::data_dir() + "/thumbs";
    util::mkdirs(dir);
    std::string thumb = dir + "/" + addr + ".png";
    if (!hypr::capture(win, thumb)) thumb.clear();
    hypr::stash(addr);
    shelf_.add_window(addr, win["class"].string(), win["title"].string(), thumb);
    shelf_.save();
}
