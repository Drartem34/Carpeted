#include "util.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

namespace util {

std::string env(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}

std::string home() { return env("HOME", "/tmp"); }

std::string runtime_dir() { return env("XDG_RUNTIME_DIR", "/tmp"); }

std::string state_dir() {
    return env("XDG_STATE_HOME", home() + "/.local/state") + "/carpeted";
}

std::string data_dir() {
    return env("XDG_DATA_HOME", home() + "/.local/share") + "/carpeted";
}

std::string config_dir() {
    return env("XDG_CONFIG_HOME", home() + "/.config") + "/carpeted";
}

std::string user_dir(const std::string& key, const std::string& fallback) {
    std::string cfg = env("XDG_CONFIG_HOME", home() + "/.config") + "/user-dirs.dirs";
    std::string text;
    if (read_file(cfg, text)) {
        std::istringstream in(text);
        std::string line, want = "XDG_" + key + "_DIR=";
        while (std::getline(in, line)) {
            if (line.rfind(want, 0) != 0) continue;
            std::string v = line.substr(want.size());
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            if (v.rfind("$HOME", 0) == 0) v = home() + v.substr(5);
            if (!v.empty()) return v;
        }
    }
    return home() + "/" + fallback;
}

bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string& path) {
    if (path.empty() || is_dir(path)) return true;
    mkdirs(dirname(path));
    return ::mkdir(path.c_str(), 0755) == 0 || is_dir(path);
}

std::string basename(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    auto k = p.rfind('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}

std::string dirname(const std::string& path) {
    auto k = path.rfind('/');
    if (k == std::string::npos) return ".";
    return k == 0 ? "/" : path.substr(0, k);
}

std::string extension(const std::string& path) {
    std::string b = basename(path);
    auto k = b.rfind('.');
    if (k == std::string::npos || k == 0) return "";
    std::string e = b.substr(k + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    mkdirs(dirname(path));
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string unique_path(const std::string& dir, const std::string& stem,
                        const std::string& ext) {
    std::string p = dir + "/" + stem + "." + ext;
    for (int n = 1; exists(p); ++n)
        p = dir + "/" + stem + "-" + std::to_string(n) + "." + ext;
    return p;
}

bool is_image_name(const std::string& path) {
    static const char* exts[] = {"png", "jpg", "jpeg", "webp", "gif", "bmp",
                                 "avif", "svg", "tif", "tiff", "jxl"};
    std::string e = extension(path);
    for (auto x : exts)
        if (e == x) return true;
    return false;
}

bool is_temp_name(const std::string& name) {
    if (name.empty() || name[0] == '.') return true;
    static const char* exts[] = {"part", "crdownload", "tmp", "download", "partial",
                                 "opdownload", "swp", "kate-swp"};
    std::string e = extension(name);
    for (auto x : exts)
        if (e == x) return true;
    return name.back() == '~';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::tolower(c));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string uri_to_path(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return "";
    std::string rest = uri.substr(7);
    auto slash = rest.find('/');           // skip an optional host part
    if (slash == std::string::npos) return "";
    rest = rest.substr(slash);
    std::string out;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() && hexval(rest[i + 1]) >= 0 &&
            hexval(rest[i + 2]) >= 0) {
            out += char(hexval(rest[i + 1]) * 16 + hexval(rest[i + 2]));
            i += 2;
        } else {
            out += rest[i];
        }
    }
    return out;
}

std::string path_to_uri(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string url_host(const std::string& url) {
    auto k = url.find("://");
    if (k == std::string::npos) return url;
    std::string rest = url.substr(k + 3);
    auto end = rest.find_first_of("/?#");
    std::string host = rest.substr(0, end);
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    return host;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool looks_like_url(const std::string& text) {
    std::string t = trim(text);
    if (t.find_first_of(" \n\t") != std::string::npos) return false;
    return t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0;
}

static std::vector<char*> cargv(const std::vector<std::string>& argv) {
    std::vector<char*> v;
    for (auto& a : argv) v.push_back(const_cast<char*>(a.c_str()));
    v.push_back(nullptr);
    return v;
}

void spawn_detached(const std::vector<std::string>& argv) {
    auto v = cargv(argv);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            int null = open("/dev/null", O_RDWR);
            dup2(null, 0); dup2(null, 1); dup2(null, 2);
            execvp(v[0], v.data());
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

int run_wait(const std::vector<std::string>& argv, int timeout_ms) {
    auto v = cargv(argv);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        execvp(v[0], v.data());
        _exit(127);
    }
    int64_t end = now_ms() + timeout_ms;
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0 || now_ms() > end) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return -1;
        }
        usleep(5000);
    }
}

bool have_program(const std::string& name) {
    std::string path = env("PATH", "/usr/bin:/bin");
    std::istringstream in(path);
    std::string dir;
    while (std::getline(in, dir, ':'))
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
    return false;
}

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string random_id() {
    static std::mt19937_64 rng(std::random_device{}());
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)rng());
    return std::string(buf, 12);
}

}  // namespace util
