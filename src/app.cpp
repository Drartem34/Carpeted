#include "app.hpp"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>

#include "hypr.hpp"
#include "util.hpp"

namespace {

constexpr double STEP = 1.0 / 60.0;
constexpr float DRAG_THRESHOLD = 6;

float ease_out(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
float ease_in_out(float t) { return t * t * (3 - 2 * t); }

}  // namespace

// ------------------------------------------------------------------ config
Config Config::load() {
    Config c;
    std::string text;
    if (!util::read_file(util::config_dir() + "/config", text)) return c;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = util::trim(line.substr(0, line.find('#')));
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(line.substr(0, eq)), v = util::trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        auto f = [&] { return std::strtof(v.c_str(), nullptr); };
        if (k == "layer") c.layer = v;
        else if (k == "output") c.output = v;
        else if (k == "height") c.height = std::clamp(f(), 0.15f, 1.f);
        else if (k == "rug_width") c.rug_width = f();
        else if (k == "rug_height") c.rug_height = f();
        else if (k == "rug_cell") c.rug_cell = std::max(6.f, f());
        else if (k == "iterations") c.iterations = std::max(1, int(f()));
        else if (k == "lift_height") c.lift_height = f();
        else if (k == "focal") c.focal = std::max(200.f, f());
        else if (k == "unroll_time") c.unroll_time = f();
        else if (k == "roll_time") c.roll_time = f();
        else if (k == "shade") c.shade = v == "true" || v == "1" || v == "yes";
        else if (k == "recent_limit") c.shelf.recent_limit = int(f());
        else if (k == "watch") {
            c.shelf.watch.clear();
            std::istringstream ws(v);
            std::string d;
            while (std::getline(ws, d, ','))
                if (!util::trim(d).empty()) c.shelf.watch.push_back(util::trim(d));
        } else {
            std::fprintf(stderr, "carpeted: unknown config key '%s'\n", k.c_str());
        }
    }
    return c;
}

// --------------------------------------------------------------------- app
App::App(const Config& cfg) : cfg_(cfg), wl_(this), shelf_(cfg.shelf) {
    shelf_.load();
    renderer_.focal = cfg_.focal;
}

App::~App() {
    if (shelf_.dirty) shelf_.save();
}

bool App::busy() const {
    return anim_ || dirty_ || grab_ >= 0 || carry_ ||
           (state_ == State::Open && cloth_ && !cloth_->sleeping());
}

int App::run(int cmd_fd, const volatile sig_atomic_t* stop) {
    if (!wl_.connect()) {
        std::fprintf(stderr, "carpeted: cannot connect to the Wayland display\n");
        return 1;
    }
    if (!wl_.has_layer_shell()) {
        std::fprintf(stderr, "carpeted: the compositor has no wlr-layer-shell\n");
        return 1;
    }
    std::string pending;     // partial command line from the daemon
    while (!quit_ && !*stop) {
        std::vector<pollfd> fds;
        fds.push_back({wl_.fd(), POLLIN, 0});
        fds.push_back({cmd_fd, POLLIN, 0});
        fds.push_back({shelf_.watch_fd(), POLLIN, 0});
        auto pipes = wl_.pipe_fds();
        for (int p : pipes) fds.push_back({p, POLLIN, 0});

        while (!wl_.prepare_read()) wl_.dispatch_pending();
        int n = poll(fds.data(), fds.size(), -1);   // sleeps until something happens
        wl_.read_events(n > 0 && (fds[0].revents & POLLIN));
        wl_.dispatch_pending();
        if (n <= 0) continue;

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            ssize_t len = read(cmd_fd, buf, sizeof buf);
            if (len <= 0) {
                hide();               // the daemon went away: roll up and leave
                if (state_ == State::Hidden) break;
            } else {
                pending.append(buf, size_t(len));
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string line = util::trim(pending.substr(0, nl));
                    pending.erase(0, nl + 1);
                    if (!line.empty()) command(line);
                }
            }
        }
        if (fds[2].revents & POLLIN) {
            if (shelf_.handle_watch([this](const Item& it) { return under_rug(it); })) {
                forget_removed();
                redraw();
            }
        }
        for (size_t k = 0; k < pipes.size(); ++k)
            if (fds[3 + k].revents & (POLLIN | POLLHUP)) wl_.handle_pipe(pipes[k]);
        wl_.flush();
    }
    if (state_ != State::Hidden) finish_hide();
    shelf_.save();
    return 0;
}

// ---------------------------------------------------------------- commands
void App::command(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "toggle") {
        if (state_ == State::Open || state_ == State::Unrolling) hide();
        else show();
    } else if (cmd == "show") {
        show();
    } else if (cmd == "hide") {
        hide();
    } else if (cmd == "stash-window") {
        stash_window();
    } else if (cmd == "shake") {
        shake();
    } else if (cmd == "ping") {
    } else if (cmd == "quit") {
        quit_ = true;
    } else if (cfg_.debug && cmd.rfind("debug-", 0) == 0) {
        // test hooks: synthetic input without a real pointer device
        float x = 0, y = 0;
        in >> x >> y;
        if (cmd == "debug-move") on_pointer_motion(x, y);
        else if (cmd == "debug-press") { on_pointer_motion(x, y); on_pointer_button(BTN_LEFT, true); }
        else if (cmd == "debug-release") { on_pointer_motion(x, y); on_pointer_button(BTN_LEFT, false); }
        else if (cmd == "debug-right") { on_pointer_motion(x, y); on_pointer_button(BTN_RIGHT, true); on_pointer_button(BTN_RIGHT, false); }
        else if (cmd == "debug-text" || cmd == "debug-uri") {
            std::string rest;
            std::getline(in, rest);
            add_from_data(cmd == "debug-uri" ? "text/uri-list" : "text/plain", util::trim(rest), x, y);
        }
    } else if (!cmd.empty()) {
        std::fprintf(stderr, "carpeted: unknown command '%s'\n", cmd.c_str());
    }
}

void App::show() {
    if (state_ == State::Open || state_ == State::Unrolling) return;
    if (state_ == State::Rolling) {           // changed our mind halfway
        start_anim(false);
        return;
    }
    std::string output = cfg_.output;
    int height = 420;
    hypr::Monitor mon;
    if (hypr::focused_monitor(mon)) {
        if (output.empty()) output = mon.name;
        height = int(mon.height / std::max(0.1, mon.scale) * cfg_.height);
    }
    if (hypr::available()) {
        auto live = hypr::client_addresses();
        shelf_.prune(&live);
    } else {
        shelf_.prune(nullptr);
    }
    forget_removed();
    state_ = State::Unrolling;
    if (!wl_.show(output, std::max(220, height), cfg_.layer)) {
        std::fprintf(stderr, "carpeted: cannot create the surface\n");
        wl_.hide();
        state_ = State::Hidden;
    }
    // the animation starts in on_configure, once we know our size
}

void App::hide() {
    if (state_ == State::Hidden || state_ == State::Rolling) return;
    grab_ = -1;
    carry_ = nullptr;
    press_item_ = nullptr;
    left_down_ = false;
    if (!cloth_ || !gl_ready_) {
        finish_hide();
        return;
    }
    cloth_->unpin();
    start_anim(true);
}

void App::finish_hide() {
    if (gl_ready_ && wl_.make_current()) renderer_.destroy(shelf_.items);
    gl_ready_ = false;
    anim_ = false;
    frame_pending_ = false;
    wl_.hide();
    state_ = State::Hidden;
    shelf_.save();
    quit_ = true;     // this process only lives while the rug is out
}

void App::shake() {
    if (state_ != State::Open || !cloth_) return;
    cloth_->unpin();
    grab_ = -1;
    cloth_->flat_pose(home_x_, home_y_, 30, pose_);
    cloth_->set_pose(pose_);
    redraw();
}

void App::stash_window() {
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
    if (state_ == State::Hidden) shelf_.save();
    else redraw();
}

// ------------------------------------------------------------------- scene
void App::setup_scene(int w, int h) {
    float rw = std::min(cfg_.rug_width, w * 0.62f);
    float rh = std::min(cfg_.rug_height, h * 0.7f);
    int cols = std::max(8, int(std::lround(rw / cfg_.rug_cell))) + 1;
    int rows = std::max(6, int(std::lround(rh / cfg_.rug_cell))) + 1;
    home_x_ = (w - rw) / 2;
    home_y_ = h - rh - std::max(26.f, h * 0.07f);
    cloth_ = std::make_unique<Cloth>(rw, rh, cols, rows, home_x_, home_y_);
    cloth_->iterations = cfg_.iterations;
    scene_w_ = w;
    scene_h_ = h;
    shelf_.width = float(w);
    shelf_.height = float(h);
    shelf_.rug = {home_x_, home_y_, home_x_ + rw, home_y_ + rh};
    shelf_.clamp();
    cloth_->rolled_pose(home_x_, home_y_, 1, pose_);
    cloth_->set_pose(pose_);
}

void App::on_configure(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!wl_.make_current()) return;
    bool new_scene = !cloth_ || w != scene_w_ || h != scene_h_;
    if (new_scene) setup_scene(w, h);
    if (!gl_ready_) {
        gl_ready_ = renderer_.init(*cloth_);
        renderer_.focal = cfg_.focal;
        if (!gl_ready_) {
            std::fprintf(stderr, "carpeted: OpenGL ES 3 setup failed\n");
            finish_hide();
            return;
        }
    } else if (new_scene) {
        renderer_.set_cloth(*cloth_);
    }
    update_obstacles();
    if (state_ == State::Unrolling && (!anim_ || new_scene)) start_anim(false);
    redraw();
}

void App::on_closed() { finish_hide(); }

void App::on_scale_changed() {
    // cards get re-rendered at the new scale on the next frame
    redraw();
}

void App::start_anim(bool roll) {
    anim_ = true;
    anim_roll_ = roll;
    anim_t0_ = util::now_ms();
    anim_from_ = cloth_->pos;
    state_ = roll ? State::Rolling : State::Unrolling;
    if (!roll) {
        cloth_->rolled_pose(home_x_, home_y_, 1, pose_);
        cloth_->set_pose(pose_);
    }
    redraw();
}

void App::step_anim() {
    float dur = anim_roll_ ? cfg_.roll_time : cfg_.unroll_time;
    float p = std::min(1.f, float(util::now_ms() - anim_t0_) / 1000.f / std::max(dur, 0.01f));
    if (!anim_roll_) {
        cloth_->rolled_pose(home_x_, home_y_, 1 - ease_out(p), pose_);
    } else {
        float blend = ease_in_out(std::min(1.f, p / 0.3f));
        cloth_->rolled_pose(home_x_, home_y_, ease_in_out(p), pose_);
        for (size_t k = 0; k < pose_.size(); ++k) {
            const V3& a = anim_from_[k];
            V3& b = pose_[k];
            b = {a.x + (b.x - a.x) * blend, a.y + (b.y - a.y) * blend, a.z + (b.z - a.z) * blend};
        }
    }
    cloth_->set_pose(pose_);
    if (p >= 1) {
        anim_ = false;
        if (anim_roll_) {
            finish_hide();
        } else {
            state_ = State::Open;
            last_step_ms_ = util::now_ms();
            accum_ = 0;
            cloth_->wake();
            update_obstacles();
        }
    }
}

void App::simulate() {
    int64_t now = util::now_ms();
    accum_ += std::min(0.1, (now - last_step_ms_) / 1000.0);
    last_step_ms_ = now;
    int steps = 0;
    while (accum_ >= STEP && steps < 3) {
        if (grab_ >= 0) {
            grab_z_ = std::min(cfg_.lift_height, grab_z_ + cfg_.lift_height * float(STEP) * 6);
            // keep the grabbed point under the cursor despite the perspective
            float cx = scene_w_ / 2.f, cy = scene_h_ / 2.f;
            float s = cfg_.focal / std::max(cfg_.focal - grab_z_, 1.f);
            float tx = std::clamp(cx + (mx_ - cx) / s, 0.f, float(scene_w_));
            float ty = std::clamp(cy + (my_ - cy) / s, 0.f, float(scene_h_) - 4);
            cloth_->pin(grab_, {tx, ty, grab_z_});
        }
        cloth_->step(float(STEP));
        accum_ -= STEP;
        ++steps;
    }
    if (steps == 3) accum_ = 0;
    if (steps) update_obstacles();
}

void App::update_obstacles() {
    if (!cloth_) return;
    std::vector<Rect> rects;
    for (auto& it : shelf_.items) {
        if (it.get() == carry_ || it.get() == dnd_item_) continue;
        float x0 = it->x - it->w / 2, y0 = it->y - it->h / 2;
        float x1 = it->x + it->w / 2, y1 = it->y + it->h / 2;
        if (it->kind == Kind::File) {       // the icon, not its label
            x0 = it->x - 26; x1 = it->x + 26; y1 = y0 + 56;
        }
        rects.push_back({x0 + 6, y0 + 6, x1 - 6, y1 - 6});
    }
    cloth_->set_obstacles(rects, 10);
}

void App::forget_removed() {
    for (auto& it : shelf_.removed) {
        if (it.get() == selected_) selected_ = nullptr;
        if (it.get() == press_item_) press_item_ = nullptr;
        if (it.get() == carry_) carry_ = nullptr;
        if (it.get() == dnd_item_) dnd_item_ = nullptr;
    }
    if (gl_ready_ && !shelf_.removed.empty() && wl_.make_current())
        for (auto& it : shelf_.removed) renderer_.forget(*it);
    shelf_.removed.clear();
}

// ----------------------------------------------------------------- drawing
void App::redraw() {
    dirty_ = true;
    if (!frame_pending_) draw();
}

void App::on_frame(uint32_t) {
    frame_pending_ = false;
    if (state_ == State::Hidden) return;
    if (anim_) step_anim();
    else if (state_ == State::Open && (grab_ >= 0 || !cloth_->sleeping())) simulate();
    if (state_ == State::Hidden) return;       // the roll-up just finished
    if (busy()) draw();
    else if (shelf_.dirty) shelf_.save();     // gone quiet: a good moment to save
}

void App::draw() {
    if (!gl_ready_ || !wl_.configured() || !wl_.make_current()) return;
    dirty_ = false;
    RenderParams p;
    p.fb_w = wl_.buffer_width();
    p.fb_h = wl_.buffer_height();
    p.view_w = float(wl_.width());
    p.view_h = float(wl_.height());
    p.scale = wl_.scale();
    p.shade_floor = cfg_.shade;
    p.selected = selected_;
    p.carried = carry_;
    p.hidden = dnd_item_;
    renderer_.render(p, *cloth_, shelf_.items);
    wl_.request_frame();                  // tells us when the next frame is due
    frame_pending_ = true;
    wl_.swap();
}

// ----------------------------------------------------------------- picking
int App::pick_rug(float x, float y) const {
    if (!cloth_ || state_ != State::Open) return -1;
    const Cloth& c = *cloth_;
    float cx = scene_w_ / 2.f, cy = scene_h_ / 2.f;
    auto proj = [&](int k, float& sx, float& sy) {
        float s = cfg_.focal / std::max(cfg_.focal - c.pos[k].z, 1.f);
        sx = cx + (c.pos[k].x - cx) * s;
        sy = cy + (c.pos[k].y - cy) * s;
    };
    int best = -1;
    float best_z = -1e9f;
    for (size_t t = 0; t + 2 < c.triangles.size(); t += 3) {
        int a = c.triangles[t], b = c.triangles[t + 1], d = c.triangles[t + 2];
        float ax, ay, bx, by, dx, dy;
        proj(a, ax, ay);
        proj(b, bx, by);
        proj(d, dx, dy);
        float c1 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        float c2 = (dx - bx) * (y - by) - (dy - by) * (x - bx);
        float c3 = (ax - dx) * (y - dy) - (ay - dy) * (x - dx);
        bool inside = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
        if (!inside) continue;
        float z = (c.pos[a].z + c.pos[b].z + c.pos[d].z) / 3;
        if (z <= best_z) continue;
        best_z = z;
        float da = std::hypot(ax - x, ay - y), db = std::hypot(bx - x, by - y),
              dd = std::hypot(dx - x, dy - y);
        best = da <= db && da <= dd ? a : (db <= dd ? b : d);
    }
    return best;
}

bool App::under_rug(const Item& it) const {
    if (state_ == State::Open) return covered(it.x, it.y);
    const Rect& r = shelf_.rug;       // rolled up: it comes back flat
    return it.x > r.x0 && it.x < r.x1 && it.y > r.y0 && it.y < r.y1;
}

// ------------------------------------------------------------------- input
void App::on_pointer_motion(float x, float y) {
    mx_ = x;
    my_ = y;
    if (state_ != State::Open) return;
    if (carry_) {
        carry_->x = x + carry_dx_;
        carry_->y = y + carry_dy_;
        redraw();
        return;
    }
    if (press_item_ && left_down_ &&
        std::hypot(x - press_x_, y - press_y_) > DRAG_THRESHOLD) {
        Item* it = press_item_;
        press_item_ = nullptr;
        begin_drag_out(it);
        return;
    }
    if (grab_ < 0 && !left_down_)
        wl_.set_cursor(covered(x, y) ? Cursor::Grab
                                     : shelf_.item_at(x, y) ? Cursor::Pointer : Cursor::Default);
}

void App::on_pointer_leave() {
    if (!left_down_) wl_.set_cursor(Cursor::Default);
}

void App::on_pointer_button(uint32_t button, bool pressed) {
    if (state_ != State::Open) return;
    float x = mx_, y = my_;
    if (button == BTN_LEFT) {
        if (pressed) {
            int64_t now = util::now_ms();
            bool dbl = now - last_click_ms_ < 400 &&
                       std::hypot(x - last_click_x_, y - last_click_y_) < DRAG_THRESHOLD;
            last_click_ms_ = dbl ? 0 : now;
            last_click_x_ = x;
            last_click_y_ = y;
            left_down_ = true;
            if (dbl) click(BTN_LEFT, x, y, true);
            else press(x, y);
        } else {
            left_down_ = false;
            release();
        }
    } else if (button == BTN_RIGHT && pressed) {
        click(BTN_RIGHT, x, y, false);
    }
}

void App::press(float x, float y) {
    press_x_ = x;
    press_y_ = y;
    int hit = pick_rug(x, y);
    if (hit >= 0) {
        grab_ = hit;
        grab_z_ = cloth_->pos[hit].z;
        wl_.set_cursor(Cursor::Grabbing);
        last_step_ms_ = util::now_ms();
        redraw();
        return;
    }
    Item* it = shelf_.item_at(x, y);
    selected_ = it;
    if (it) {
        shelf_.raise(it);
        if (it->kind == Kind::Window) {
            carry_ = it;
            carry_dx_ = it->x - x;
            carry_dy_ = it->y - y;
            update_obstacles();
        } else {
            press_item_ = it;
        }
    }
    redraw();
}

void App::release() {
    press_item_ = nullptr;
    if (grab_ >= 0) {
        grab_ = -1;
        cloth_->unpin();
        wl_.set_cursor(Cursor::Grab);
        last_step_ms_ = util::now_ms();
        redraw();
    }
    if (carry_) {
        Item* it = carry_;
        carry_ = nullptr;
        if (my_ < -8 || mx_ < -8 || mx_ > scene_w_ + 8) {   // dragged off the floor
            restore_window(it);
        } else {
            shelf_.clamp();
            shelf_.dirty = true;
        }
        update_obstacles();
        redraw();
    }
}

void App::click(uint32_t button, float x, float y, bool dbl) {
    if (covered(x, y)) {
        if (button == BTN_LEFT && dbl) shake();
        return;
    }
    Item* it = shelf_.item_at(x, y);
    if (!it) {
        selected_ = nullptr;
        redraw();
        return;
    }
    if (it->kind == Kind::Window) {
        restore_window(it);
    } else if (button == BTN_RIGHT) {
        shelf_.remove(it);
        forget_removed();
        redraw();
    } else if (dbl) {
        if (it->kind == Kind::Text) wl_.copy_to_clipboard(it->text);
        else if (it->kind == Kind::Link) util::spawn_detached({"xdg-open", it->url});
        else util::spawn_detached({"xdg-open", it->path});
    }
}

void App::on_key(uint32_t sym, bool ctrl) {
    if (sym == XKB_KEY_Escape) {
        hide();
    } else if (ctrl && (sym == XKB_KEY_v || sym == XKB_KEY_V)) {
        float x = mx_ >= 0 ? mx_ : scene_w_ / 2.f, y = my_ >= 0 ? my_ : scene_h_ / 3.f;
        wl_.paste(x, y);
    } else if (sym == XKB_KEY_Delete && selected_) {
        if (selected_->kind == Kind::Window) restore_window(selected_);
        else {
            shelf_.remove(selected_);
            forget_removed();
            redraw();
        }
    }
}

// --------------------------------------------------------- drag out / in
void App::begin_drag_out(Item* it) {
    std::map<std::string, std::string> offers;
    offers[CARD_MIME] = it->id;
    auto text = [&](const std::string& t) {
        for (auto m : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}) offers[m] = t;
    };
    switch (it->kind) {
    case Kind::File:
    case Kind::Image:
        offers["text/uri-list"] = util::path_to_uri(it->path) + "\r\n";
        text(it->path);
        break;
    case Kind::Link:
        offers["text/uri-list"] = it->url + "\r\n";
        text(it->url);
        break;
    case Kind::Text:
        text(it->text);
        break;
    case Kind::Window:
        return;
    }
    int icon_scale = std::max(1, int(std::ceil(wl_.scale())));
    CardImage icon = render_card(*it, float(icon_scale));
    dnd_item_ = it;
    dnd_dx_ = it->x - press_x_;
    dnd_dy_ = it->y - press_y_;
    int hot_x = int(press_x_ - (it->x - it->w / 2)), hot_y = int(press_y_ - (it->y - it->h / 2));
    wl_.start_drag(offers, icon, hot_x, hot_y, icon_scale);
    left_down_ = false;               // the compositor owns the pointer now
    update_obstacles();
    redraw();
}

void App::on_drag_finished() {
    dnd_item_ = nullptr;
    update_obstacles();
    redraw();
}

void App::on_dnd_motion(float, float) {}
void App::on_dnd_leave() {}

void App::on_internal_drop(float x, float y) {
    if (!dnd_item_) return;
    dnd_item_->x = x + dnd_dx_;
    dnd_item_->y = y + dnd_dy_;
    shelf_.clamp();
    shelf_.dirty = true;
    update_obstacles();
    redraw();
}

void App::on_data(const std::string& mime, const std::string& data, float x, float y) {
    add_from_data(mime, data, x, y);
}

void App::add_from_data(const std::string& mime, const std::string& data, float x, float y) {
    int added = 0;
    if (mime == "text/uri-list") {
        std::istringstream in(data);
        std::string line;
        while (std::getline(in, line)) {
            line = util::trim(line);
            if (line.empty() || line[0] == '#') continue;
            float px = x + added * 16, py = y + added * 12;
            std::string path = util::uri_to_path(line);
            if (!path.empty()) {
                if (Item* old = shelf_.find_path(path)) {
                    old->x = px;
                    old->y = py;
                } else {
                    shelf_.add_file(path, px, py);
                }
            } else {
                shelf_.add_link(line, px, py);
            }
            ++added;
        }
    } else if (mime.rfind("image/", 0) == 0) {
        std::string ext = mime.substr(6);
        if (ext == "jpeg") ext = "jpg";
        added = shelf_.add_image_bytes(data, ext, x, y) != nullptr;
    } else {
        added = shelf_.add_text(data, x, y) != nullptr;
    }
    if (!added) return;
    shelf_.clamp();
    shelf_.dirty = true;
    update_obstacles();
    if (state_ == State::Hidden) shelf_.save();
    else redraw();
}

void App::restore_window(Item* it) {
    double cx = 0, cy = 0;
    bool have = hypr::cursor_pos(cx, cy);
    hypr::restore(it->address, have, cx, cy);
    shelf_.remove(it);
    forget_removed();
    redraw();
}
