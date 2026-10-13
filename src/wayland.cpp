#include "wayland.hpp"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

#include <EGL/eglext.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
// the protocol has an argument called "namespace", a C++ keyword
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

namespace {

Wl* W(void* data) { return static_cast<Wl*>(data); }

const char* TEXT_MIMES[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain",
                            "STRING", "TEXT"};

// ------------------------------------------------------------------- output
const wl_output_listener output_listener = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t,
                   const char*, const char*, int32_t) {},
    .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    .done = [](void*, wl_output*) {},
    .scale =
        [](void* data, wl_output* o, int32_t s) {
            for (auto& out : W(data)->outputs_)
                if (out.output == o) out.scale = s;
        },
    .name =
        [](void* data, wl_output* o, const char* name) {
            for (auto& out : W(data)->outputs_)
                if (out.output == o) out.name = name;
            W(data)->output_names_.clear();
            for (auto& out : W(data)->outputs_) W(data)->output_names_.push_back(out.name);
        },
    .description = [](void*, wl_output*, const char*) {},
};

// ------------------------------------------------------------------ pointer
const wl_pointer_listener pointer_listener = {
    .enter =
        [](void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
            Wl* w = W(data);
            w->enter_serial_ = serial;
            w->pointer_inside_ = true;
            w->px_ = wl_fixed_to_double(x);
            w->py_ = wl_fixed_to_double(y);
            w->apply_cursor();
            w->handler_->on_pointer_motion(w->px_, w->py_);
        },
    .leave =
        [](void* data, wl_pointer*, uint32_t, wl_surface*) {
            W(data)->pointer_inside_ = false;
            W(data)->handler_->on_pointer_leave();
        },
    .motion =
        [](void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            Wl* w = W(data);
            w->px_ = wl_fixed_to_double(x);
            w->py_ = wl_fixed_to_double(y);
            w->handler_->on_pointer_motion(w->px_, w->py_);
        },
    .button =
        [](void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
            Wl* w = W(data);
            w->last_serial_ = serial;
            if (state == WL_POINTER_BUTTON_STATE_PRESSED) w->button_serial_ = serial;
            w->handler_->on_pointer_button(button, state == WL_POINTER_BUTTON_STATE_PRESSED);
        },
    .axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
    .frame = [](void*, wl_pointer*) {},
    .axis_source = [](void*, wl_pointer*, uint32_t) {},
    .axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
    .axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {},
    .axis_value120 = [](void*, wl_pointer*, uint32_t, int32_t) {},
    .axis_relative_direction = [](void*, wl_pointer*, uint32_t, uint32_t) {},
};

// ----------------------------------------------------------------- keyboard
const wl_keyboard_listener keyboard_listener = {
    .keymap =
        [](void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
            Wl* w = W(data);
            if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
                close(fd);
                return;
            }
            char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
            close(fd);
            if (map == MAP_FAILED) return;
            if (!w->xkb_) w->xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            xkb_keymap* km = xkb_keymap_new_from_string(w->xkb_, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(map, size);
            if (!km) return;
            if (w->xkb_state_) xkb_state_unref(w->xkb_state_);
            if (w->keymap_) xkb_keymap_unref(w->keymap_);
            w->keymap_ = km;
            w->xkb_state_ = xkb_state_new(km);
        },
    .enter = [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},
    .leave = [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
    .key =
        [](void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
            Wl* w = W(data);
            w->last_serial_ = serial;
            if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !w->xkb_state_) return;
            xkb_keysym_t sym = xkb_state_key_get_one_sym(w->xkb_state_, key + 8);
            bool ctrl = xkb_state_mod_name_is_active(w->xkb_state_, XKB_MOD_NAME_CTRL,
                                                     XKB_STATE_MODS_EFFECTIVE) > 0;
            w->handler_->on_key(sym, ctrl);
        },
    .modifiers =
        [](void* data, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock,
           uint32_t group) {
            if (W(data)->xkb_state_)
                xkb_state_update_mask(W(data)->xkb_state_, dep, lat, lock, 0, 0, group);
        },
    .repeat_info = [](void*, wl_keyboard*, int32_t, int32_t) {},
};

// --------------------------------------------------------------------- seat
const wl_seat_listener seat_listener = {
    .capabilities =
        [](void* data, wl_seat* seat, uint32_t caps) {
            Wl* w = W(data);
            if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer_) {
                w->pointer_ = wl_seat_get_pointer(seat);
                wl_pointer_add_listener(w->pointer_, &pointer_listener, w);
                if (w->cursor_shape_mgr_)
                    w->cursor_device_ =
                        wp_cursor_shape_manager_v1_get_pointer(w->cursor_shape_mgr_, w->pointer_);
            }
            if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->keyboard_) {
                w->keyboard_ = wl_seat_get_keyboard(seat);
                wl_keyboard_add_listener(w->keyboard_, &keyboard_listener, w);
            }
        },
    .name = [](void*, wl_seat*, const char*) {},
};

// ------------------------------------------------------------ data offers
const wl_data_offer_listener offer_listener = {
    .offer =
        [](void* data, wl_data_offer* o, const char* mime) {
            W(data)->offers_[o].mimes.push_back(mime);
        },
    .source_actions =
        [](void* data, wl_data_offer* o, uint32_t actions) {
            W(data)->offers_[o].source_actions = actions;
        },
    .action = [](void*, wl_data_offer*, uint32_t) {},
};

const wl_data_device_listener device_listener = {
    .data_offer =
        [](void* data, wl_data_device*, wl_data_offer* o) {
            W(data)->offers_[o] = {};
            wl_data_offer_add_listener(o, &offer_listener, data);
        },
    .enter =
        [](void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x,
           wl_fixed_t y, wl_data_offer* o) {
            Wl* w = W(data);
            if (w->dnd_offer_ && w->dnd_offer_ != o) w->destroy_offer(w->dnd_offer_);
            w->dnd_offer_ = o;
            w->dnd_x_ = wl_fixed_to_double(x);
            w->dnd_y_ = wl_fixed_to_double(y);
            if (!o || surface != w->surface_) return;
            auto& info = w->offers_[o];
            w->dnd_internal_ =
                std::find(info.mimes.begin(), info.mimes.end(), CARD_MIME) != info.mimes.end();
            w->dnd_mime_ = w->dnd_internal_ ? CARD_MIME : w->pick_mime(info);
            wl_data_offer_accept(o, serial, w->dnd_mime_.empty() ? nullptr : w->dnd_mime_.c_str());
            if (w->ddm_version_ >= 3) {
                uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
                uint32_t move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
                // never "move": the source would delete the original file
                wl_data_offer_set_actions(o, copy | (w->dnd_internal_ ? move : 0), copy);
            }
            w->handler_->on_dnd_motion(w->dnd_x_, w->dnd_y_);
        },
    .leave =
        [](void* data, wl_data_device*) {
            Wl* w = W(data);
            if (w->dnd_offer_) w->destroy_offer(w->dnd_offer_);
            w->dnd_offer_ = nullptr;
            w->handler_->on_dnd_leave();
        },
    .motion =
        [](void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            Wl* w = W(data);
            w->dnd_x_ = wl_fixed_to_double(x);
            w->dnd_y_ = wl_fixed_to_double(y);
            w->handler_->on_dnd_motion(w->dnd_x_, w->dnd_y_);
        },
    .drop =
        [](void* data, wl_data_device*) {
            Wl* w = W(data);
            wl_data_offer* o = w->dnd_offer_;
            w->dnd_offer_ = nullptr;
            if (!o) return;
            if (w->dnd_internal_) {
                w->handler_->on_internal_drop(w->dnd_x_, w->dnd_y_);
                if (w->ddm_version_ >= 3) wl_data_offer_finish(o);
                w->destroy_offer(o);
            } else if (!w->dnd_mime_.empty()) {
                w->receive(o, w->dnd_mime_, w->dnd_x_, w->dnd_y_, true);
            } else {
                w->destroy_offer(o);
            }
            w->handler_->on_dnd_leave();
        },
    .selection =
        [](void* data, wl_data_device*, wl_data_offer* o) {
            Wl* w = W(data);
            if (w->selection_offer_ && w->selection_offer_ != o)
                w->destroy_offer(w->selection_offer_);
            w->selection_offer_ = o;
        },
};

// ----------------------------------------------------------- data sources
const wl_data_source_listener source_listener = {
    .target = [](void*, wl_data_source*, const char*) {},
    .send =
        [](void* data, wl_data_source* src, const char* mime, int32_t fd) {
            Wl* w = W(data);
            for (auto* s : w->sources_) {
                if (s->source != src) continue;
                auto it = s->data.find(mime);
                if (it != s->data.end()) {
                    int flags = fcntl(fd, F_GETFL);
                    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                    const std::string& d = it->second;
                    size_t off = 0;
                    while (off < d.size()) {
                        ssize_t n = write(fd, d.data() + off, d.size() - off);
                        if (n <= 0) break;
                        off += size_t(n);
                    }
                }
            }
            close(fd);
        },
    .cancelled =
        [](void* data, wl_data_source* src) {
            Wl* w = W(data);
            for (auto* s : w->sources_)
                if (s->source == src) {
                    bool drag = s->drag;
                    w->destroy_source(s);
                    if (drag) w->handler_->on_drag_finished();
                    return;
                }
        },
    .dnd_drop_performed = [](void*, wl_data_source*) {},
    .dnd_finished =
        [](void* data, wl_data_source* src) {
            Wl* w = W(data);
            for (auto* s : w->sources_)
                if (s->source == src) {
                    w->destroy_source(s);
                    w->handler_->on_drag_finished();
                    return;
                }
        },
    .action = [](void*, wl_data_source*, uint32_t) {},
};

// ------------------------------------------------------------- the surface
const zwlr_layer_surface_v1_listener layer_listener = {
    .configure =
        [](void* data, zwlr_layer_surface_v1* ls, uint32_t serial, uint32_t width,
           uint32_t height) {
            Wl* w = W(data);
            zwlr_layer_surface_v1_ack_configure(ls, serial);
            w->width_ = int(width);
            w->height_ = int(height);
            w->configured_ = true;
            w->resize_buffers();
            w->handler_->on_configure(w->width_, w->height_);
        },
    .closed = [](void* data, zwlr_layer_surface_v1*) { W(data)->handler_->on_closed(); },
};

const wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale =
        [](void* data, wp_fractional_scale_v1*, uint32_t scale120) {
            Wl* w = W(data);
            float s = scale120 / 120.f;
            if (std::fabs(s - w->scale_) > 1e-3f) {
                w->scale_ = s;
                w->resize_buffers();
                w->handler_->on_scale_changed();
            }
        },
};

const wl_surface_listener surface_listener = {
    .enter = [](void*, wl_surface*, wl_output*) {},
    .leave = [](void*, wl_surface*, wl_output*) {},
    .preferred_buffer_scale =
        [](void* data, wl_surface*, int32_t factor) {
            Wl* w = W(data);
            if (w->fractional_ || factor == w->int_scale_) return;
            w->int_scale_ = factor;
            w->scale_ = float(factor);
            w->resize_buffers();
            w->handler_->on_scale_changed();
        },
    .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
};

const wl_callback_listener frame_listener = {
    .done =
        [](void* data, wl_callback* cb, uint32_t time) {
            Wl* w = W(data);
            wl_callback_destroy(cb);
            if (w->frame_cb_ == cb) w->frame_cb_ = nullptr;
            w->handler_->on_frame(time);
        },
};

// ----------------------------------------------------------------- registry
const wl_registry_listener registry_listener = {
    .global =
        [](void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
            Wl* w = W(data);
            auto bind = [&](const wl_interface* i, uint32_t want) {
                return wl_registry_bind(reg, name, i, std::min(version, want));
            };
            std::string n = iface;
            if (n == wl_compositor_interface.name) {
                w->compositor_version_ = std::min(version, 6u);
                w->compositor_ = static_cast<wl_compositor*>(bind(&wl_compositor_interface, 6));
            } else if (n == wl_shm_interface.name) {
                w->shm_ = static_cast<wl_shm*>(bind(&wl_shm_interface, 1));
            } else if (n == wl_seat_interface.name && !w->seat_) {
                w->seat_ = static_cast<wl_seat*>(bind(&wl_seat_interface, 5));
                wl_seat_add_listener(w->seat_, &seat_listener, w);
            } else if (n == wl_output_interface.name) {
                auto* o = static_cast<wl_output*>(bind(&wl_output_interface, 4));
                w->outputs_.push_back({o, name, "", 1});
                wl_output_add_listener(o, &output_listener, w);
            } else if (n == wl_data_device_manager_interface.name) {
                w->ddm_version_ = std::min(version, 3u);
                w->ddm_ = static_cast<wl_data_device_manager*>(
                    bind(&wl_data_device_manager_interface, 3));
            } else if (n == zwlr_layer_shell_v1_interface.name) {
                w->layer_shell_version_ = std::min(version, 4u);
                w->layer_shell_ =
                    static_cast<zwlr_layer_shell_v1*>(bind(&zwlr_layer_shell_v1_interface, 4));
            } else if (n == wp_viewporter_interface.name) {
                w->viewporter_ = static_cast<wp_viewporter*>(bind(&wp_viewporter_interface, 1));
            } else if (n == wp_fractional_scale_manager_v1_interface.name) {
                w->fractional_mgr_ = static_cast<wp_fractional_scale_manager_v1*>(
                    bind(&wp_fractional_scale_manager_v1_interface, 1));
            } else if (n == wp_cursor_shape_manager_v1_interface.name) {
                w->cursor_shape_mgr_ = static_cast<wp_cursor_shape_manager_v1*>(
                    bind(&wp_cursor_shape_manager_v1_interface, 1));
            }
        },
    .global_remove =
        [](void* data, wl_registry*, uint32_t name) {
            Wl* w = W(data);
            for (size_t k = 0; k < w->outputs_.size(); ++k)
                if (w->outputs_[k].global == name) {
                    if (w->current_output_ == &w->outputs_[k]) w->current_output_ = nullptr;
                    wl_output_destroy(w->outputs_[k].output);
                    w->outputs_.erase(w->outputs_.begin() + k);
                    return;
                }
        },
};

}  // namespace

// ======================================================================== Wl
Wl::~Wl() {
    hide();
    for (auto* s : std::vector<Source*>(sources_)) destroy_source(s);
    for (auto& r : reads_) close(r.fd);
    if (display_) wl_display_disconnect(display_);
}

bool Wl::connect() {
    display_ = wl_display_connect(nullptr);
    if (!display_) return false;
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);
    wl_display_roundtrip(display_);               // globals
    if (seat_ && ddm_) {
        data_device_ = wl_data_device_manager_get_data_device(ddm_, seat_);
        wl_data_device_add_listener(data_device_, &device_listener, this);
    }
    wl_display_roundtrip(display_);               // seat caps, output names
    return compositor_ != nullptr;
}

int Wl::fd() const { return wl_display_get_fd(display_); }

bool Wl::prepare_read() {
    if (wl_display_prepare_read(display_) != 0) return false;
    wl_display_flush(display_);
    return true;
}

void Wl::read_events(bool readable) {
    if (readable) wl_display_read_events(display_);
    else wl_display_cancel_read(display_);
}

void Wl::dispatch_pending() { wl_display_dispatch_pending(display_); }

void Wl::flush() { wl_display_flush(display_); }

int Wl::buffer_width() const { return std::max(1, int(std::lround(width_ * scale_))); }
int Wl::buffer_height() const { return std::max(1, int(std::lround(height_ * scale_))); }

// ------------------------------------------------------------ show / hide
bool Wl::show(const std::string& output_name, int height, const std::string& layer) {
    if (surface_ || !layer_shell_) return false;
    current_output_ = nullptr;
    for (auto& o : outputs_)
        if (!output_name.empty() && o.name == output_name) current_output_ = &o;

    surface_ = wl_compositor_create_surface(compositor_);
    if (compositor_version_ >= 6) wl_surface_add_listener(surface_, &surface_listener, this);
    scale_ = 1;
    int_scale_ = 1;
    if (fractional_mgr_ && viewporter_) {
        viewport_ = wp_viewporter_get_viewport(viewporter_, surface_);
        fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(fractional_mgr_, surface_);
        wp_fractional_scale_v1_add_listener(fractional_, &fractional_listener, this);
    } else if (current_output_ && compositor_version_ < 6) {
        int_scale_ = current_output_->scale;
        scale_ = float(int_scale_);
    }
    uint32_t lyr = layer == "overlay" ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                                      : ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell_, surface_, current_output_ ? current_output_->output : nullptr, lyr,
        "carpeted");
    zwlr_layer_surface_v1_add_listener(layer_surface_, &layer_listener, this);
    zwlr_layer_surface_v1_set_size(layer_surface_, 0, uint32_t(height));
    zwlr_layer_surface_v1_set_anchor(layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, 0);
    if (layer_shell_version_ >= 4)
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    configured_ = false;
    wl_surface_commit(surface_);
    return egl_up();
}

void Wl::hide() {
    if (!surface_) return;
    egl_down();
    if (frame_cb_) wl_callback_destroy(frame_cb_);
    frame_cb_ = nullptr;
    if (egl_window_) wl_egl_window_destroy(egl_window_);
    egl_window_ = nullptr;
    if (fractional_) wp_fractional_scale_v1_destroy(fractional_);
    fractional_ = nullptr;
    if (viewport_) wp_viewport_destroy(viewport_);
    viewport_ = nullptr;
    zwlr_layer_surface_v1_destroy(layer_surface_);
    layer_surface_ = nullptr;
    wl_surface_destroy(surface_);
    surface_ = nullptr;
    configured_ = false;
    pointer_inside_ = false;
    wl_display_flush(display_);
}

void Wl::resize_buffers() {
    if (!surface_ || !configured_) return;
    int bw = buffer_width(), bh = buffer_height();
    if (viewport_) {
        wp_viewport_set_destination(viewport_, width_, height_);
    } else {
        wl_surface_set_buffer_scale(surface_, int_scale_);
        bw = width_ * int_scale_;
        bh = height_ * int_scale_;
    }
    if (!egl_window_) {
        egl_window_ = wl_egl_window_create(surface_, bw, bh);
        egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_,
                                              (EGLNativeWindowType)egl_window_, nullptr);
    } else {
        wl_egl_window_resize(egl_window_, bw, bh, 0, 0);
    }
}

// ---------------------------------------------------------------------- EGL
bool Wl::egl_up() {
    auto get_platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    egl_display_ = get_platform ? get_platform(EGL_PLATFORM_WAYLAND_KHR, display_, nullptr)
                                : eglGetDisplay((EGLNativeDisplayType)display_);
    if (egl_display_ == EGL_NO_DISPLAY || !eglInitialize(egl_display_, nullptr, nullptr)) {
        std::fprintf(stderr, "carpeted: cannot initialise EGL\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16,
                            EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(egl_display_, attrs, &egl_config_, 1, &n) || n < 1) {
        std::fprintf(stderr, "carpeted: no suitable EGL config\n");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_context_ == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "carpeted: cannot create a GLES 3 context\n");
        return false;
    }
    return true;
}

void Wl::egl_down() {
    if (egl_display_ == EGL_NO_DISPLAY) return;
    eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface_ != EGL_NO_SURFACE) eglDestroySurface(egl_display_, egl_surface_);
    if (egl_context_ != EGL_NO_CONTEXT) eglDestroyContext(egl_display_, egl_context_);
    eglTerminate(egl_display_);             // gives the driver's memory back
    eglReleaseThread();
    egl_surface_ = EGL_NO_SURFACE;
    egl_context_ = EGL_NO_CONTEXT;
    egl_display_ = EGL_NO_DISPLAY;
}

bool Wl::make_current() {
    if (egl_surface_ == EGL_NO_SURFACE) return false;
    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) return false;
    eglSwapInterval(egl_display_, 0);       // we pace ourselves with frame callbacks
    return true;
}

void Wl::request_frame() {
    if (!surface_ || frame_cb_) return;
    frame_cb_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_cb_, &frame_listener, this);
}

void Wl::swap() {
    if (egl_surface_ != EGL_NO_SURFACE) eglSwapBuffers(egl_display_, egl_surface_);
}

// ------------------------------------------------------------------- cursor
void Wl::set_cursor(Cursor c) {
    if (c == cursor_) return;
    cursor_ = c;
    if (pointer_inside_) apply_cursor();
}

void Wl::apply_cursor() {
    if (!pointer_) return;
    if (cursor_device_) {
        uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
        if (cursor_ == Cursor::Pointer) shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
        if (cursor_ == Cursor::Grab) shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
        if (cursor_ == Cursor::Grabbing) shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
        wp_cursor_shape_device_v1_set_shape(cursor_device_, enter_serial_, shape);
        return;
    }
    // Fallback for compositors without cursor-shape: a plain arrow.
    if (!shm_) return;
    if (!cursor_theme_) {
        cursor_theme_ = wl_cursor_theme_load(nullptr, 24, shm_);
        cursor_surface_ = wl_compositor_create_surface(compositor_);
    }
    wl_cursor* cur = cursor_theme_ ? wl_cursor_theme_get_cursor(cursor_theme_, "left_ptr") : nullptr;
    if (!cur) return;
    wl_cursor_image* img = cur->images[0];
    wl_surface_attach(cursor_surface_, wl_cursor_image_get_buffer(img), 0, 0);
    wl_surface_damage(cursor_surface_, 0, 0, int(img->width), int(img->height));
    wl_surface_commit(cursor_surface_);
    wl_pointer_set_cursor(pointer_, enter_serial_, cursor_surface_, int(img->hotspot_x),
                          int(img->hotspot_y));
}

// ------------------------------------------------------------ data transfer
std::string Wl::pick_mime(const Offer& o) const {
    auto has = [&](const std::string& m) {
        return std::find(o.mimes.begin(), o.mimes.end(), m) != o.mimes.end();
    };
    if (has("text/uri-list")) return "text/uri-list";
    for (auto m : {"image/png", "image/jpeg", "image/webp", "image/gif"})
        if (has(m)) return m;
    for (auto m : TEXT_MIMES)
        if (has(m)) return m;
    return "";
}

void Wl::destroy_offer(wl_data_offer* o) {
    for (auto& r : reads_)
        if (r.offer == o) return;          // still being read; destroyed afterwards
    offers_.erase(o);
    if (selection_offer_ == o) selection_offer_ = nullptr;
    if (dnd_offer_ == o) dnd_offer_ = nullptr;
    wl_data_offer_destroy(o);
}

void Wl::receive(wl_data_offer* offer, const std::string& mime, float x, float y,
                 bool finish_offer) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return;
    wl_data_offer_receive(offer, mime.c_str(), fds[1]);
    close(fds[1]);
    wl_display_flush(display_);
    reads_.push_back({fds[0], finish_offer ? offer : nullptr, mime, "", x, y});
}

std::vector<int> Wl::pipe_fds() const {
    std::vector<int> v;
    for (auto& r : reads_) v.push_back(r.fd);
    return v;
}

void Wl::handle_pipe(int fd) {
    for (size_t k = 0; k < reads_.size(); ++k) {
        PendingRead& r = reads_[k];
        if (r.fd != fd) continue;
        char buf[65536];
        while (true) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
                r.data.append(buf, size_t(n));
                if (r.data.size() > (64u << 20)) n = 0;     // 64 MB is plenty
                else continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;   // more later
            break;                                                      // EOF or error
        }
        PendingRead done = std::move(r);
        reads_.erase(reads_.begin() + k);
        close(done.fd);
        if (done.offer) {
            if (ddm_version_ >= 3) wl_data_offer_finish(done.offer);
            destroy_offer(done.offer);
        }
        handler_->on_data(done.mime, done.data, done.x, done.y);
        return;
    }
}

void Wl::destroy_source(Source* s) {
    sources_.erase(std::remove(sources_.begin(), sources_.end(), s), sources_.end());
    wl_data_source_destroy(s->source);
    if (s->icon) wl_surface_destroy(s->icon);
    if (s->icon_buffer) wl_buffer_destroy(s->icon_buffer);
    delete s;
}

static wl_buffer* shm_buffer(wl_shm* shm, const CardImage& img) {
    size_t size = size_t(img.stride) * img.height;
    int fd = memfd_create("carpeted-icon", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(size)) != 0) {
        if (fd >= 0) close(fd);
        return nullptr;
    }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    std::memcpy(map, img.bgra.data(), size);
    munmap(map, size);
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, int(size));
    wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, img.width, img.height, img.stride,
                                               WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

void Wl::start_drag(const std::map<std::string, std::string>& offers, const CardImage& icon,
                    int hot_x, int hot_y, int icon_scale) {
    if (!data_device_ || !surface_) return;
    auto* s = new Source{wl_data_device_manager_create_data_source(ddm_), offers, true};
    for (auto& [mime, _] : offers) wl_data_source_offer(s->source, mime.c_str());
    if (ddm_version_ >= 3)
        wl_data_source_set_actions(s->source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_source_add_listener(s->source, &source_listener, this);
    sources_.push_back(s);
    if (shm_ && icon.width > 0) {
        s->icon = wl_compositor_create_surface(compositor_);
        s->icon_buffer = shm_buffer(shm_, icon);
    }
    wl_data_device_start_drag(data_device_, s->source, surface_, s->icon, button_serial_);
    if (s->icon && s->icon_buffer) {
        wl_surface_set_buffer_scale(s->icon, icon_scale);
        wl_surface_attach(s->icon, s->icon_buffer, -hot_x, -hot_y);
        wl_surface_damage(s->icon, 0, 0, icon.width, icon.height);
        wl_surface_commit(s->icon);
    }
    wl_display_flush(display_);
}

void Wl::copy_to_clipboard(const std::string& text) {
    if (!data_device_) return;
    for (auto* s : std::vector<Source*>(sources_))
        if (!s->drag) destroy_source(s);
    auto* s = new Source{wl_data_device_manager_create_data_source(ddm_), {}, false};
    for (auto m : TEXT_MIMES) {
        s->data[m] = text;
        wl_data_source_offer(s->source, m);
    }
    wl_data_source_add_listener(s->source, &source_listener, this);
    sources_.push_back(s);
    wl_data_device_set_selection(data_device_, s->source, last_serial_);
    wl_display_flush(display_);
}

void Wl::paste(float x, float y) {
    if (!selection_offer_) return;
    std::string mime = pick_mime(offers_[selection_offer_]);
    if (!mime.empty()) receive(selection_offer_, mime, x, y, false);
}
