#include "shelf.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <random>

#include "util.hpp"

namespace {

std::mt19937& rng() {
    static std::mt19937 r(std::random_device{}());
    return r;
}

float uniform(float a, float b) {
    return std::uniform_real_distribution<float>(a, b)(rng());
}

std::string state_file() { return util::state_dir() + "/shelf.json"; }

}  // namespace

Shelf::Shelf(const ShelfConfig& cfg) : cfg_(cfg) {
    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    std::vector<std::pair<std::string, bool>> dirs;
    std::string downloads = util::user_dir("DOWNLOAD", "Downloads");
    std::string pictures = util::user_dir("PICTURES", "Pictures");
    for (auto& w : cfg_.watch) {
        if (w == "downloads") {
            dirs.push_back({downloads, true});
        } else if (w == "screenshots") {
            for (auto env : {"XDG_SCREENSHOTS_DIR", "HYPRSHOT_DIR", "GRIMBLAST_DIR"}) {
                std::string d = util::env(env);
                if (!d.empty()) dirs.push_back({d, false});
            }
            dirs.push_back({pictures, false});
            dirs.push_back({pictures + "/Screenshots", false});
            dirs.push_back({util::home() + "/Screenshots", false});
        } else {
            std::string d = w.rfind("~/", 0) == 0 ? util::home() + w.substr(1) : w;
            dirs.push_back({d, true});
        }
    }
    if (inotify_fd_ >= 0) {
        for (auto& [d, any] : dirs) {
            if (!util::is_dir(d)) continue;
            int wd = inotify_add_watch(inotify_fd_, d.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
            if (wd < 0) continue;
            bool dup = false;
            for (auto& w : watches_)
                if (w.wd == wd) { w.any_file = w.any_file || any; dup = true; }
            if (!dup) watches_.push_back({wd, d, any});
        }
    }
}

Shelf::~Shelf() {
    if (inotify_fd_ >= 0) close(inotify_fd_);
}

// ----------------------------------------------------------- persistence
void Shelf::load() {
    std::string text;
    if (!util::read_file(state_file(), text)) return;
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok) return;
    items.clear();
    if (j["size"].type == Json::Array) {
        width = float(j["size"][0].number(width));
        height = float(j["size"][1].number(height));
    }
    if (j["rug"].type == Json::Array)
        rug = {float(j["rug"][0].number()), float(j["rug"][1].number()),
               float(j["rug"][2].number()), float(j["rug"][3].number())};
    for (auto& e : j["items"].arr) {
        auto it = std::make_unique<Item>();
        if (!Item::from_json(e, *it)) continue;
        Item* raw = it.get();
        items.push_back(std::move(it));
        if (!e.has("on_rug")) {           // saved by an older version: onto the rug
            float u, v;
            free_rug_spot(raw->w, raw->h, u, v);
            put_on_rug(raw, u, v);
        }
    }
}

void Shelf::save() {
    Json j = Json::object();
    j.obj["items"] = Json::array();
    j.obj["size"] = Json::array();
    j.obj["size"].arr = {Json::of(double(width)), Json::of(double(height))};
    j.obj["rug"] = Json::array();
    j.obj["rug"].arr = {Json::of(double(rug.x0)), Json::of(double(rug.y0)),
                        Json::of(double(rug.x1)), Json::of(double(rug.y1))};
    for (auto& it : items) j.obj["items"].arr.push_back(it->to_json());
    util::write_file_atomic(state_file(), j.dump());
    dirty = false;
}

// ------------------------------------------------------------- placement
void Shelf::free_rug_spot(float w, float h, float& bu, float& bv) {
    // a free place on the rug, away from the other cards lying on it
    const float W = rug_w(), H = rug_h(), m = 6;
    float best = -1e9f;
    bu = W / 2;
    bv = H / 2;
    for (int k = 0; k < 150; ++k) {
        float u = uniform(std::min(w / 2 + m, W / 2), std::max(W - w / 2 - m, W / 2 + 0.1f));
        float v = uniform(std::min(h / 2 + m, H / 2), std::max(H - h / 2 - m, H / 2 + 0.1f));
        float nearest = 1e9f;
        for (auto& it : items)
            if (it->on_rug)
                nearest = std::min(nearest, std::max(std::fabs(u - it->u) / (w + it->w) * 2,
                                                     std::fabs(v - it->v) / (h + it->h) * 2));
        if (nearest > best) {
            best = nearest;
            bu = u;
            bv = v;
        }
    }
}

void Shelf::put_on_rug(Item* it, float u, float v) {
    it->on_rug = true;
    it->u = std::clamp(u, std::min(it->w / 2, rug_w() / 2), std::max(rug_w() - it->w / 2, rug_w() / 2));
    it->v = std::clamp(v, std::min(it->h / 2, rug_h() / 2), std::max(rug_h() - it->h / 2, rug_h() / 2));
    dirty = true;
}

void Shelf::put_on_floor(Item* it, float x, float y) {
    it->on_rug = false;
    it->x = std::clamp(x, it->w / 2, std::max(it->w / 2, width - it->w / 2));
    it->y = std::clamp(y, it->h / 2, std::max(it->h / 2, height - it->h / 2));
    dirty = true;
}

Item* Shelf::place(std::unique_ptr<Item> it, float x, float y) {
    it->id = util::random_id();
    it->added = double(std::time(nullptr));
    it->angle = uniform(-0.12f, 0.12f);
    it->set_size();
    Item* raw = it.get();
    items.push_back(std::move(it));
    if (x == NAN_POS) {
        float u, v;
        free_rug_spot(raw->w, raw->h, u, v);
        put_on_rug(raw, u, v);
    } else {
        put_on_floor(raw, x, y);   // the app moves it onto the rug if it landed there
    }
    dirty = true;
    return raw;
}

Item* Shelf::add_file(const std::string& path, float x, float y, bool automatic) {
    auto it = std::make_unique<Item>();
    it->kind = !util::is_dir(path) && util::is_image_name(path) ? Kind::Image : Kind::File;
    it->path = path;
    it->automatic = automatic;
    return place(std::move(it), x, y);
}

Item* Shelf::add_link(const std::string& url, float x, float y) {
    auto it = std::make_unique<Item>();
    it->kind = Kind::Link;
    it->url = util::trim(url);
    return place(std::move(it), x, y);
}

Item* Shelf::add_text(const std::string& text, float x, float y) {
    std::string t = util::trim(text);
    if (t.empty()) return nullptr;
    if (util::looks_like_url(t)) return add_link(t, x, y);
    auto it = std::make_unique<Item>();
    it->kind = Kind::Text;
    it->text = t.substr(0, 20000);
    return place(std::move(it), x, y);
}

Item* Shelf::add_image_bytes(const std::string& bytes, const std::string& ext, float x, float y) {
    std::string dir = util::data_dir() + "/pictures";
    util::mkdirs(dir);
    char stem[64];
    std::time_t now = std::time(nullptr);
    std::strftime(stem, sizeof stem, "picture-%Y%m%d-%H%M%S", std::localtime(&now));
    std::string path = util::unique_path(dir, stem, ext);
    if (!util::write_file_atomic(path, bytes)) return nullptr;
    Item* it = add_file(path, x, y);
    it->owned = path;
    return it;
}

Item* Shelf::add_window(const std::string& address, const std::string& wclass,
                        const std::string& title, const std::string& thumb) {
    auto it = std::make_unique<Item>();
    it->kind = Kind::Window;
    it->address = address;
    it->wclass = wclass;
    it->title = title;
    it->thumb = thumb;
    return place(std::move(it), NAN_POS, NAN_POS);
}

void Shelf::remove(Item* item) {
    for (size_t k = 0; k < items.size(); ++k)
        if (items[k].get() == item) {
            // things only we kept are deleted with their card
            for (auto* f : {&item->owned, &item->thumb})
                if (!f->empty()) ::unlink(f->c_str());
            removed.push_back(std::move(items[k]));
            items.erase(items.begin() + k);
            dirty = true;
            return;
        }
}

Item* Shelf::find_path(const std::string& path) {
    for (auto& it : items)
        if (!it->path.empty() && it->path == path) return it.get();
    return nullptr;
}

Item* Shelf::item_at(float x, float y) {
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        if ((*it)->contains(x, y)) return it->get();
    return nullptr;
}

void Shelf::raise(Item* item) {
    for (size_t k = 0; k < items.size(); ++k)
        if (items[k].get() == item) {
            auto p = std::move(items[k]);
            items.erase(items.begin() + k);
            items.push_back(std::move(p));
            return;
        }
}

void Shelf::clamp() {
    for (auto& it : items) {
        if (it->on_rug) {
            put_on_rug(it.get(), it->u, it->v);
        } else {
            it->x = std::clamp(it->x, it->w / 2, std::max(it->w / 2, width - it->w / 2));
            it->y = std::clamp(it->y, it->h / 2, std::max(it->h / 2, height - it->h / 2));
        }
    }
}

bool Shelf::prune(const std::vector<std::string>* live_windows) {
    std::vector<Item*> gone;
    for (auto& it : items) {
        if (!it->exists()) gone.push_back(it.get());
        else if (it->kind == Kind::Window && live_windows &&
                 std::find(live_windows->begin(), live_windows->end(), it->address) ==
                     live_windows->end())
            gone.push_back(it.get());
    }
    for (auto* g : gone) remove(g);
    return !gone.empty();
}

// ---------------------------------------------------------------- watching
void Shelf::drain_watch() {
    char buf[8192];
    while (read(inotify_fd_, buf, sizeof buf) > 0) {}
}

bool Shelf::handle_watch(const std::function<bool(const Item&)>& hidden) {
    alignas(inotify_event) char buf[8192];
    bool added = false;
    while (true) {
        ssize_t n = read(inotify_fd_, buf, sizeof buf);
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            if (!ev->len || (ev->mask & IN_ISDIR)) continue;
            std::string name = ev->name;
            if (util::is_temp_name(name)) continue;
            const Watch* w = nullptr;
            for (auto& x : watches_)
                if (x.wd == ev->wd) w = &x;
            if (!w) continue;
            std::string path = w->dir + "/" + name;
            if (!w->any_file && !util::is_image_name(path)) continue;
            if (find_path(path) || !util::exists(path)) continue;
            add_file(path, NAN_POS, NAN_POS, true);
            added = true;
        }
    }
    if (added) trim(hidden);
    return added;
}

void Shelf::trim(const std::function<bool(const Item&)>& hidden) {
    std::vector<Item*> autos;
    for (auto& it : items)
        if (it->automatic && !hidden(*it)) autos.push_back(it.get());
    std::sort(autos.begin(), autos.end(),
              [](Item* a, Item* b) { return a->added < b->added; });
    int extra = int(autos.size()) - cfg_.recent_limit;
    for (int k = 0; k < extra; ++k) remove(autos[k]);
}
