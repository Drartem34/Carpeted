// The application: rolls the rug out from the bottom of the screen on
// command, runs the physics while something moves and sleeps otherwise.
#pragma once
#include <csignal>
#include <memory>
#include <string>
#include <vector>

#include "cloth.hpp"
#include "renderer.hpp"
#include "shelf.hpp"
#include "wayland.hpp"

struct Config {
    std::string layer = "top";        // "top" or "overlay"
    std::string output;               // empty: the focused monitor
    float height = 0.42f;             // share of the screen height
    float rug_width = 900, rug_height = 300;
    float rug_cell = 15;              // physics grid spacing, px
    int iterations = 10;
    float lift_height = 60;
    float focal = 1800;
    float unroll_time = 0.9f, roll_time = 0.55f;
    bool shade = true;                // dark see-through band behind the floor
    float relax_delay = 4;            // s of quiet before the rug straightens itself (0: never)
    ShelfConfig shelf;
    bool debug = false;               // accept debug-* commands (tests)

    static Config load();             // ~/.config/carpeted/config
};

class App : public WlHandler {
public:
    explicit App(const Config& cfg);
    ~App() override;
    // Runs until the rug is rolled up again; commands arrive on cmd_fd.
    int run(int cmd_fd, const volatile sig_atomic_t* stop);

    // WlHandler
    void on_configure(int width, int height) override;
    void on_closed() override;
    void on_frame(uint32_t time_ms) override;
    void on_scale_changed() override;
    void on_pointer_motion(float x, float y) override;
    void on_pointer_button(uint32_t button, bool pressed) override;
    void on_pointer_leave() override;
    void on_key(uint32_t keysym, bool ctrl) override;
    void on_dnd_motion(float x, float y) override;
    void on_dnd_leave() override;
    void on_internal_drop(float x, float y) override;
    void on_data(const std::string& mime, const std::string& data, float x, float y) override;
    void on_drag_finished() override;

private:
    enum class State { Hidden, Unrolling, Open, Rolling };

    Config cfg_;
    Wl wl_;
    Shelf shelf_;
    Renderer renderer_;
    bool gl_ready_ = false;
    bool quit_ = false;
    State state_ = State::Hidden;

    // the rug, laid out for the current surface size
    std::unique_ptr<Cloth> cloth_;
    float home_x_ = 0, home_y_ = 0;
    int scene_w_ = 0, scene_h_ = 0;

    // animation / simulation clock
    bool anim_ = false, anim_roll_ = false;
    int64_t anim_t0_ = 0;
    std::vector<V3> anim_from_, pose_;
    int64_t last_step_ms_ = 0;
    double accum_ = 0;
    bool frame_pending_ = false;
    bool dirty_ = false;

    // pointer
    float mx_ = -1, my_ = -1;
    bool left_down_ = false;
    float press_x_ = 0, press_y_ = 0;
    Item* press_item_ = nullptr;      // a card pressed, maybe about to be dragged out
    int grab_ = -1;                   // rug particle in hand
    float grab_z_ = 0;
    Item* carry_ = nullptr;           // a window card in hand
    float carry_dx_ = 0, carry_dy_ = 0;
    Item* dnd_item_ = nullptr;        // a card being dragged out (not drawn)
    float dnd_dx_ = 0, dnd_dy_ = 0;
    Item* selected_ = nullptr;
    int64_t last_click_ms_ = 0;
    float last_click_x_ = -1e9, last_click_y_ = -1e9;

    // commands
    void command(const std::string& cmd);
    void show();
    void hide();
    void finish_hide();
    void stash_window();
    void shake();

    void setup_scene(int w, int h);
    void start_anim(bool roll);
    void step_anim();
    void simulate();
    bool busy() const;
    void redraw();
    void draw();
    void update_obstacles();
    void forget_removed();

    struct RugHit {
        int particle = -1;
        float z = 0, u = 0, v = 0, nz = 1;
    };
    RugHit pick_rug_hit(float x, float y) const;
    int pick_rug(float x, float y) const { return pick_rug_hit(x, y).particle; }
    bool covered(float x, float y) const { return pick_rug(x, y) >= 0; }
    // the card under the cursor, if the rug does not cover it
    Item* card_at(float x, float y) const;
    bool card_screen(const Item& it, float& sx, float& sy, float& z) const;
    void place_at(Item* it, float x, float y);   // on the rug if it landed on it
    bool under_rug(const Item& it) const;
    void press(float x, float y);
    void release();
    void click(uint32_t button, float x, float y, bool double_click);
    void begin_drag_out(Item* it);
    void restore_window(Item* it);
    void add_from_data(const std::string& mime, const std::string& data, float x, float y);

    // the rug straightening itself: an invisible hand slowly folds back the
    // part that is furthest from lying flat
    bool relax_pending_ = false, relax_active_ = false;
    int relax_k_ = -1;
    V3 relax_from_, relax_to_;
    int64_t relax_t0_ = 0, last_touch_ms_ = 0;
    float relax_dur_ = 0, relax_lift_ = 0;
    int timer_ms() const;                 // poll timeout until the next relax check
    void on_timer();
    void step_relax();
};
