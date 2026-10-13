// The always-running part: a few MB, no graphics at all.  It listens for
// commands, notices new downloads/screenshots, stashes windows, and starts
// the rug process (Wayland + GL) only while the rug is out.
#pragma once
#include <sys/types.h>

#include <csignal>
#include <string>

#include "app.hpp"
#include "shelf.hpp"

class Daemon {
public:
    explicit Daemon(const Config& cfg);
    int run(int listen_fd, const std::string& first_command, const volatile sig_atomic_t* stop);

private:
    Config cfg_;
    Shelf shelf_;
    pid_t child_ = -1;
    int child_fd_ = -1;
    bool quit_ = false;

    void handle(const std::string& cmd);
    bool forward(const std::string& cmd);
    void spawn_ui();
    void child_gone();
    void stash_window();
};
