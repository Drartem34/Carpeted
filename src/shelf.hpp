// The cards on the floor: persistence, placement and the folders we watch
// (new downloads and screenshots show up by themselves).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cards.hpp"
#include "cloth.hpp"

struct ShelfConfig {
    std::vector<std::string> watch = {"downloads", "screenshots"};
    int recent_limit = 30;
};

class Shelf {
public:
    explicit Shelf(const ShelfConfig& cfg);
    ~Shelf();

    std::vector<std::unique_ptr<Item>> items;
    std::vector<std::unique_ptr<Item>> removed;   // waiting for GL cleanup
    float width = 1280, height = 400;            // floor size
    Rect rug{0, 0, 0, 0};                        // where the flat rug lies

    void load();
    void save();
    bool dirty = false;

    Item* add_file(const std::string& path, float x, float y, bool automatic = false);
    Item* add_link(const std::string& url, float x, float y);
    Item* add_text(const std::string& text, float x, float y);
    Item* add_image_bytes(const std::string& bytes, const std::string& ext, float x, float y);
    Item* add_window(const std::string& address, const std::string& wclass,
                     const std::string& title, const std::string& thumb);
    void remove(Item* item);
    Item* find_path(const std::string& path);

    Item* item_at(float x, float y);
    void raise(Item* item);
    void clamp();
    bool prune(const std::vector<std::string>* live_windows);

    // inotify: watch fd for the event loop; returns true if cards were added
    int watch_fd() const { return inotify_fd_; }
    bool handle_watch(const std::function<bool(const Item&)>& hidden);
    void drain_watch();      // read and ignore pending events

    static constexpr float NAN_POS = -1e9f;   // "find me a free spot"

private:
    ShelfConfig cfg_;
    int inotify_fd_ = -1;
    struct Watch {
        int wd;
        std::string dir;
        bool any_file;   // downloads: everything; pictures: only images
    };
    std::vector<Watch> watches_;
    Item* place(std::unique_ptr<Item> item, float x, float y);
    void free_spot(float w, float h, float& x, float& y);
    void trim(const std::function<bool(const Item&)>& hidden);
};
