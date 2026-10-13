#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace util {

std::string env(const char* name, const std::string& def = "");
std::string home();
std::string runtime_dir();
std::string state_dir();        // $XDG_STATE_HOME/carpeted
std::string data_dir();         // $XDG_DATA_HOME/carpeted
std::string config_dir();       // $XDG_CONFIG_HOME/carpeted
// XDG user dirs ("DOWNLOAD", "PICTURES", ...) from user-dirs.dirs
std::string user_dir(const std::string& key, const std::string& fallback);

bool exists(const std::string& path);
bool is_dir(const std::string& path);
bool mkdirs(const std::string& path);
std::string basename(const std::string& path);
std::string dirname(const std::string& path);
std::string extension(const std::string& path);    // lower case, no dot
bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);
std::string unique_path(const std::string& dir, const std::string& stem,
                        const std::string& ext);

bool is_image_name(const std::string& path);
bool is_temp_name(const std::string& name);

std::string uri_to_path(const std::string& uri);   // "" if not a file:// uri
std::string path_to_uri(const std::string& path);
std::string url_host(const std::string& url);
bool looks_like_url(const std::string& text);
std::string trim(const std::string& s);

// Start a program fully detached (double fork); never blocks.
void spawn_detached(const std::vector<std::string>& argv);
// Run a program and wait for it (with a timeout); returns exit status or -1.
int run_wait(const std::vector<std::string>& argv, int timeout_ms);
bool have_program(const std::string& name);

int64_t now_ms();
std::string random_id();

}  // namespace util
