// Native Wayland client: one layer-shell surface along the bottom of the
// screen, pointer + keyboard, drag and drop in both directions, clipboard,
// fractional scaling and an EGL/GLES3 context that only exists while the
// rug is out.
#pragma once
#include <EGL/egl.h>
#include <wayland-client.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cards.hpp"

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct wl_egl_window;
struct wl_cursor_theme;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

enum class Cursor { Default, Pointer, Grab, Grabbing };

// Mime type marking our own cards, so dropping one back onto the floor
// moves it instead of adding a copy.
constexpr const char* CARD_MIME = "application/x-carpeted-card";

class WlHandler {
public:
    virtual ~WlHandler() = default;
    virtual void on_configure(int width, int height) = 0;     // logical px
    virtual void on_closed() = 0;
    virtual void on_frame(uint32_t time_ms) = 0;
    virtual void on_scale_changed() = 0;
    virtual void on_pointer_motion(float x, float y) = 0;
    virtual void on_pointer_button(uint32_t button, bool pressed) = 0;
    virtual void on_pointer_leave() = 0;
    virtual void on_key(uint32_t keysym, bool ctrl) = 0;
    virtual void on_dnd_motion(float x, float y) = 0;
    virtual void on_dnd_leave() = 0;
    virtual void on_internal_drop(float x, float y) = 0;
    // data dropped on us or pasted, already read
    virtual void on_data(const std::string& mime, const std::string& data, float x, float y) = 0;
    virtual void on_drag_finished() = 0;
};

class Wl {
public:
    explicit Wl(WlHandler* h) : handler_(h) {}
    ~Wl();

    bool connect();
    int fd() const;
    // event loop helpers
    bool prepare_read();         // flush; false = events already pending
    void read_events(bool readable);
    void dispatch_pending();
    void flush();
    std::vector<int> pipe_fds() const;
    void handle_pipe(int fd);

    // surface lifecycle
    bool show(const std::string& output_name, int height, const std::string& layer);
    void hide();
    bool shown() const { return surface_ != nullptr; }
    bool configured() const { return configured_; }
    int width() const { return width_; }
    int height() const { return height_; }
    float scale() const { return scale_; }
    int buffer_width() const;
    int buffer_height() const;

    // drawing
    bool make_current();
    void request_frame();
    void swap();

    void set_cursor(Cursor c);

    // data transfer
    void start_drag(const std::map<std::string, std::string>& offers, const CardImage& icon,
                    int hot_x, int hot_y, int icon_scale);
    void copy_to_clipboard(const std::string& text);
    void paste(float x, float y);

    const std::vector<std::string>& output_names() const { return output_names_; }
    bool has_layer_shell() const { return layer_shell_ != nullptr; }

    // --- internals, public for the C callbacks -------------------------
    struct Output {
        wl_output* output;
        uint32_t global;
        std::string name;
        int scale = 1;
    };
    struct Offer {
        std::vector<std::string> mimes;
        uint32_t source_actions = 0;
    };
    struct Source {
        wl_data_source* source;
        std::map<std::string, std::string> data;
        bool drag;
        wl_surface* icon = nullptr;
        wl_buffer* icon_buffer = nullptr;
    };
    struct PendingRead {
        int fd;
        wl_data_offer* offer;    // finish + destroy when done (drops), or null
        std::string mime, data;
        float x, y;
    };

    WlHandler* handler_;
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_data_device_manager* ddm_ = nullptr;
    uint32_t ddm_version_ = 0;
    wl_data_device* data_device_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    uint32_t layer_shell_version_ = 0;
    wp_viewporter* viewporter_ = nullptr;
    wp_fractional_scale_manager_v1* fractional_mgr_ = nullptr;
    wp_cursor_shape_manager_v1* cursor_shape_mgr_ = nullptr;
    wp_cursor_shape_device_v1* cursor_device_ = nullptr;
    uint32_t compositor_version_ = 0;
    std::vector<Output> outputs_;
    std::vector<std::string> output_names_;

    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_surface_ = nullptr;
    wp_viewport* viewport_ = nullptr;
    wp_fractional_scale_v1* fractional_ = nullptr;
    wl_callback* frame_cb_ = nullptr;
    wl_egl_window* egl_window_ = nullptr;
    bool configured_ = false;
    int width_ = 0, height_ = 0;
    float scale_ = 1;
    int int_scale_ = 1;
    Output* current_output_ = nullptr;

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLConfig egl_config_ = nullptr;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;

    // input
    float px_ = 0, py_ = 0;
    uint32_t enter_serial_ = 0, last_serial_ = 0, button_serial_ = 0;
    bool pointer_inside_ = false;
    Cursor cursor_ = Cursor::Default;
    wl_cursor_theme* cursor_theme_ = nullptr;
    wl_surface* cursor_surface_ = nullptr;
    xkb_context* xkb_ = nullptr;
    xkb_keymap* keymap_ = nullptr;
    xkb_state* xkb_state_ = nullptr;

    // data transfer
    std::map<wl_data_offer*, Offer> offers_;
    wl_data_offer* dnd_offer_ = nullptr;
    wl_data_offer* selection_offer_ = nullptr;
    std::string dnd_mime_;
    bool dnd_internal_ = false;
    float dnd_x_ = 0, dnd_y_ = 0;
    std::vector<Source*> sources_;
    std::vector<PendingRead> reads_;

    void resize_buffers();
    void apply_cursor();
    bool egl_up();
    void egl_down();
    void destroy_offer(wl_data_offer* o);
    void destroy_source(Source* s);
    std::string pick_mime(const Offer& o) const;
    void receive(wl_data_offer* offer, const std::string& mime, float x, float y,
                 bool finish_offer);
};
