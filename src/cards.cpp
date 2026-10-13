#include "cards.hpp"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>

#include "util.hpp"

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::File: return "file";
    case Kind::Image: return "image";
    case Kind::Link: return "link";
    case Kind::Text: return "text";
    case Kind::Window: return "window";
    }
    return "file";
}

static bool kind_from(const std::string& s, Kind& k) {
    for (Kind c : {Kind::File, Kind::Image, Kind::Link, Kind::Text, Kind::Window})
        if (s == kind_name(c)) { k = c; return true; }
    return false;
}

void Item::set_size() {
    switch (kind) {
    case Kind::File: w = 96; h = 100; break;
    case Kind::Image: w = 128; h = 118; break;
    case Kind::Link: w = 176; h = 64; break;
    case Kind::Text: w = 132; h = 112; break;
    case Kind::Window: w = 180; h = 132; break;
    }
}

std::string Item::label() const {
    switch (kind) {
    case Kind::File:
    case Kind::Image: return util::basename(path);
    case Kind::Link: return url;
    case Kind::Text: return text;
    case Kind::Window: return title.empty() ? wclass : title;
    }
    return "";
}

bool Item::contains(float px, float py) const {
    return std::fabs(px - x) <= w / 2 && std::fabs(py - y) <= h / 2;
}

bool Item::exists() const {
    if (kind == Kind::File || kind == Kind::Image) return util::exists(path);
    return true;
}

Json Item::to_json() const {
    Json j = Json::object();
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) j.obj[k] = Json::of(v);
    };
    j.obj["id"] = Json::of(id);
    j.obj["kind"] = Json::of(std::string(kind_name(kind)));
    j.obj["x"] = Json::of(std::round(x * 10) / 10.0);
    j.obj["y"] = Json::of(std::round(y * 10) / 10.0);
    j.obj["angle"] = Json::of(std::round(angle * 1000) / 1000.0);
    j.obj["auto"] = Json::of(automatic);
    j.obj["added"] = Json::of(added);
    put("path", path);
    put("url", url);
    put("text", text);
    put("address", address);
    put("class", wclass);
    put("title", title);
    put("thumb", thumb);
    put("owned", owned);
    return j;
}

bool Item::from_json(const Json& j, Item& it) {
    if (!kind_from(j["kind"].string(), it.kind)) return false;
    it.id = j["id"].string(util::random_id());
    it.x = j["x"].number();
    it.y = j["y"].number();
    it.angle = j["angle"].number();
    it.automatic = j["auto"].boolean();
    it.added = j["added"].number(double(std::time(nullptr)));
    it.path = j["path"].string();
    it.url = j["url"].string();
    it.text = j["text"].string();
    it.address = j["address"].string();
    it.wclass = j["class"].string();
    it.title = j["title"].string();
    it.thumb = j["thumb"].string();
    it.owned = j["owned"].string();
    it.set_size();
    switch (it.kind) {
    case Kind::File:
    case Kind::Image: return !it.path.empty();
    case Kind::Link: return !it.url.empty();
    case Kind::Text: return !it.text.empty();
    case Kind::Window: return !it.address.empty();
    }
    return false;
}

// ------------------------------------------------------------------ drawing
namespace {

constexpr double ICON = 52;

void rounded(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

PangoLayout* layout(cairo_t* cr, const std::string& text, const char* font, double width,
                    int lines, PangoAlignment align = PANGO_ALIGN_CENTER) {
    PangoLayout* l = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_width(l, int(width * PANGO_SCALE));
    pango_layout_set_alignment(l, align);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_height(l, -lines);
    pango_layout_set_text(l, text.c_str(), -1);
    return l;
}

void show(cairo_t* cr, PangoLayout* l, double x, double y, double r, double g, double b) {
    cairo_move_to(cr, x, y);
    cairo_set_source_rgb(cr, r, g, b);
    pango_cairo_show_layout(cr, l);
}

// White text with a dark halo: readable over any wallpaper.
void label(cairo_t* cr, const std::string& text, const char* font, double x, double y,
           double width, int lines) {
    PangoLayout* l = layout(cr, text, font, width, lines);
    const double halo[][3] = {{0, 1, 0.9}, {1, 1, 0.5}, {-1, 1, 0.5}, {1, 0, 0.35}, {-1, 0, 0.35}};
    for (auto& h : halo) {
        cairo_move_to(cr, x + h[0], y + h[1]);
        cairo_set_source_rgba(cr, 0, 0, 0, h[2]);
        pango_cairo_show_layout(cr, l);
    }
    show(cr, l, x, y, 1, 1, 1);
    g_object_unref(l);
}

GdkPixbuf* load_pixbuf(const std::string& path, int max_w, int max_h) {
    if (path.empty()) return nullptr;
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), max_w, max_h, TRUE, &err);
    if (err) g_error_free(err);
    if (!pb) return nullptr;
    GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    if (oriented && (gdk_pixbuf_get_width(oriented) > max_w ||
                     gdk_pixbuf_get_height(oriented) > max_h)) {
        double s = std::min(double(max_w) / gdk_pixbuf_get_width(oriented),
                            double(max_h) / gdk_pixbuf_get_height(oriented));
        GdkPixbuf* sc = gdk_pixbuf_scale_simple(
            oriented, std::max(1, int(gdk_pixbuf_get_width(oriented) * s)),
            std::max(1, int(gdk_pixbuf_get_height(oriented) * s)), GDK_INTERP_BILINEAR);
        g_object_unref(oriented);
        oriented = sc;
    }
    return oriented;
}

// Paint a pixbuf of device pixels at logical (x, y).
void paint_pixbuf(cairo_t* cr, GdkPixbuf* pb, double x, double y, double scale) {
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
    const guchar* src = gdk_pixbuf_read_pixels(pb);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(s);
    unsigned char* dst = cairo_image_surface_get_data(s);
    int ds = cairo_image_surface_get_stride(s);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const guchar* p = src + yy * rs + xx * n;
            unsigned a = n == 4 ? p[3] : 255;
            uint32_t v = (a << 24) | ((p[0] * a / 255) << 16) | ((p[1] * a / 255) << 8) |
                         (p[2] * a / 255);
            std::memcpy(dst + yy * ds + xx * 4, &v, 4);
        }
    cairo_surface_mark_dirty(s);
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, 1 / scale, 1 / scale);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(s);
}

enum class FileType { Folder, Video, Audio, Archive, Pdf, Code, Doc };

FileType file_type(const std::string& path) {
    if (util::is_dir(path)) return FileType::Folder;
    std::string e = util::extension(path);
    auto in = [&](std::initializer_list<const char*> l) {
        for (auto x : l)
            if (e == x) return true;
        return false;
    };
    if (in({"mp4", "mkv", "webm", "mov", "avi", "m4v", "wmv", "flv"})) return FileType::Video;
    if (in({"mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma"})) return FileType::Audio;
    if (in({"zip", "tar", "gz", "xz", "zst", "7z", "rar", "bz2", "tgz", "deb", "rpm"}))
        return FileType::Archive;
    if (e == "pdf") return FileType::Pdf;
    if (in({"c", "cc", "cpp", "h", "hpp", "py", "js", "ts", "rs", "go", "sh", "lua", "json",
            "toml", "yaml", "yml", "conf", "ini", "html", "css", "java", "kt", "zig", "nix"}))
        return FileType::Code;
    return FileType::Doc;
}

void draw_file_icon(cairo_t* cr, const std::string& path, double x, double y, double s) {
    FileType t = file_type(path);
    if (t == FileType::Folder) {
        cairo_set_source_rgb(cr, 0.27, 0.56, 0.85);
        rounded(cr, x + s * 0.06, y + s * 0.14, s * 0.4, s * 0.2, 3);
        cairo_fill(cr);
        rounded(cr, x + s * 0.06, y + s * 0.22, s * 0.88, s * 0.64, 4);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.42, 0.72, 0.96);
        rounded(cr, x + s * 0.06, y + s * 0.32, s * 0.88, s * 0.54, 4);
        cairo_fill(cr);
        return;
    }
    // a sheet of paper with a folded corner
    double x0 = x + s * 0.16, x1 = x + s * 0.84, y0 = y + s * 0.04, y1 = y + s * 0.96,
           fold = s * 0.2;
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1 - fold, y0);
    cairo_line_to(cr, x1, y0 + fold);
    cairo_line_to(cr, x1, y1);
    cairo_line_to(cr, x0, y1);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_move_to(cr, x1 - fold, y0);
    cairo_line_to(cr, x1 - fold, y0 + fold);
    cairo_line_to(cr, x1, y0 + fold);
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.84);
    cairo_fill(cr);

    double cx = (x0 + x1) / 2, cy = y + s * 0.5;
    double r = 0.4, g = 0.45, b = 0.5;
    switch (t) {
    case FileType::Video:
        r = 0.55; g = 0.3; b = 0.85;
        cairo_move_to(cr, cx - s * 0.1, cy - s * 0.13);
        cairo_line_to(cr, cx + s * 0.14, cy);
        cairo_line_to(cr, cx - s * 0.1, cy + s * 0.13);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_fill(cr);
        break;
    case FileType::Audio:
        r = 0.9; g = 0.45; b = 0.2;
        cairo_set_source_rgb(cr, r, g, b);
        cairo_arc(cr, cx - s * 0.06, cy + s * 0.1, s * 0.07, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_rectangle(cr, cx, cy - s * 0.16, s * 0.03, s * 0.26);
        cairo_fill(cr);
        cairo_rectangle(cr, cx, cy - s * 0.16, s * 0.12, s * 0.04);
        cairo_fill(cr);
        break;
    case FileType::Archive:
        r = 0.7; g = 0.55; b = 0.3;
        cairo_set_source_rgb(cr, r, g, b);
        for (int k = 0; k < 6; ++k)
            cairo_rectangle(cr, cx - s * 0.03 + (k % 2) * s * 0.03, y0 + k * s * 0.07,
                            s * 0.03, s * 0.05);
        cairo_fill(cr);
        rounded(cr, cx - s * 0.06, y0 + s * 0.44, s * 0.12, s * 0.12, 2);
        cairo_fill(cr);
        break;
    default:
        if (t == FileType::Pdf) { r = 0.85; g = 0.2; b = 0.2; }
        if (t == FileType::Code) { r = 0.2; g = 0.6; b = 0.45; }
        cairo_set_source_rgba(cr, 0, 0, 0, 0.17);
        for (int k = 0; k < 4; ++k)
            cairo_rectangle(cr, x0 + s * 0.1, y0 + s * (0.28 + k * 0.1), s * 0.44, s * 0.035);
        cairo_fill(cr);
    }
    // coloured band with the extension
    std::string e = util::extension(path);
    if (!e.empty() && e.size() <= 5) {
        for (auto& ch : e) ch = char(std::toupper((unsigned char)ch));
        cairo_rectangle(cr, x0 - s * 0.04, y1 - s * 0.28, (x1 - x0) * 0.78, s * 0.17);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_fill(cr);
        PangoLayout* l = layout(cr, e, "Sans Bold 7", (x1 - x0) * 0.78, 1);
        show(cr, l, x0 - s * 0.04, y1 - s * 0.28 + 1, 1, 1, 1);
        g_object_unref(l);
    }
}

void draw_globe(cairo_t* cr, double cx, double cy) {
    cairo_arc(cr, cx, cy, 13, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.25, 0.5, 0.9);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 1.3);
    cairo_arc(cr, cx, cy, 9, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, 0.45, 1);
    cairo_arc(cr, 0, 0, 9, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - 9, cy);
    cairo_line_to(cr, cx + 9, cy);
    cairo_stroke(cr);
}

}  // namespace

CardImage render_card(const Item& it, float scale) {
    CardImage img;
    img.width = int(std::ceil(it.w * scale));
    img.height = int(std::ceil(it.h * scale));
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img.width, img.height);
    cairo_t* cr = cairo_create(surf);
    cairo_scale(cr, scale, scale);
    const double w = it.w, h = it.h;

    switch (it.kind) {
    case Kind::File:
        draw_file_icon(cr, it.path, (w - ICON) / 2, 4, ICON);
        label(cr, it.label(), "Sans 9", 3, ICON + 8, w - 6, 2);
        break;

    case Kind::Image: {   // a photo print with a white border
        GdkPixbuf* pb = load_pixbuf(it.path, int((w - 16) * scale), int((h - 34) * scale));
        double pw = pb ? gdk_pixbuf_get_width(pb) / scale : w - 20;
        double ph = pb ? gdk_pixbuf_get_height(pb) / scale : h - 40;
        double fx = (w - pw) / 2 - 5, fy = 3;
        cairo_rectangle(cr, fx + 1, fy + 2, pw + 10, ph + 10);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
        cairo_fill(cr);
        cairo_rectangle(cr, fx, fy, pw + 10, ph + 10);
        cairo_set_source_rgb(cr, 0.98, 0.97, 0.94);
        cairo_fill(cr);
        if (pb) {
            paint_pixbuf(cr, pb, fx + 5, fy + 5, scale);
            g_object_unref(pb);
        }
        label(cr, it.label(), "Sans 8", 3, fy + ph + 14, w - 6, 1);
        break;
    }

    case Kind::Window: {  // a small window with a title bar
        rounded(cr, 3, 3, w - 6, h - 6, 7);
        cairo_set_source_rgb(cr, 0.13, 0.14, 0.17);
        cairo_fill(cr);
        PangoLayout* l = layout(cr, it.wclass.empty() ? "window" : it.wclass, "Sans Bold 8",
                                w - 50, 1, PANGO_ALIGN_LEFT);
        show(cr, l, 12, 7, 0.92, 0.92, 0.95);
        g_object_unref(l);
        const double dots[][3] = {{0.95, 0.35, 0.3}, {0.95, 0.75, 0.25}, {0.35, 0.8, 0.4}};
        for (int k = 0; k < 3; ++k) {
            cairo_arc(cr, w - 14 - k * 11, 13, 3.5, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, dots[k][0], dots[k][1], dots[k][2]);
            cairo_fill(cr);
        }
        GdkPixbuf* pb = load_pixbuf(it.thumb, int((w - 12) * scale), int((h - 30) * scale));
        if (pb) {
            double pw = gdk_pixbuf_get_width(pb) / scale, ph = gdk_pixbuf_get_height(pb) / scale;
            paint_pixbuf(cr, pb, (w - pw) / 2, 24 + (h - 30 - ph) / 2, scale);
            g_object_unref(pb);
        } else {
            PangoLayout* t = layout(cr, it.title, "Sans 9", w - 20, 4);
            show(cr, t, 10, 34, 0.8, 0.8, 0.85);
            g_object_unref(t);
        }
        break;
    }

    case Kind::Link: {
        rounded(cr, 2, 2, w - 4, h - 4, 10);
        cairo_set_source_rgb(cr, 0.97, 0.97, 0.99);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        draw_globe(cr, 24, h / 2);
        PangoLayout* l = layout(cr, util::url_host(it.url), "Sans Bold 9", w - 52, 1,
                                PANGO_ALIGN_LEFT);
        show(cr, l, 44, 11, 0.1, 0.1, 0.15);
        g_object_unref(l);
        l = layout(cr, it.url, "Sans 8", w - 52, 2, PANGO_ALIGN_LEFT);
        show(cr, l, 44, 28, 0.35, 0.35, 0.45);
        g_object_unref(l);
        break;
    }

    case Kind::Text: {    // a sticky note
        cairo_move_to(cr, 4, 4);
        cairo_line_to(cr, w - 4, 4);
        cairo_line_to(cr, w - 4, h - 18);
        cairo_line_to(cr, w - 18, h - 4);
        cairo_line_to(cr, 4, h - 4);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 1.0, 0.92, 0.45);
        cairo_fill(cr);
        cairo_move_to(cr, w - 4, h - 18);
        cairo_line_to(cr, w - 18, h - 18);
        cairo_line_to(cr, w - 18, h - 4);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.85, 0.75, 0.3);
        cairo_fill(cr);
        PangoLayout* l = layout(cr, it.text, "Sans 8", w - 20, 6, PANGO_ALIGN_LEFT);
        show(cr, l, 10, 10, 0.2, 0.18, 0.1);
        g_object_unref(l);
        break;
    }
    }

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    img.stride = cairo_image_surface_get_stride(surf);
    const unsigned char* data = cairo_image_surface_get_data(surf);
    img.bgra.assign(data, data + size_t(img.stride) * img.height);
    cairo_surface_destroy(surf);
    return img;
}

void bgra_to_rgba(CardImage& img) {
    for (int y = 0; y < img.height; ++y) {
        uint8_t* row = img.bgra.data() + size_t(y) * img.stride;
        for (int x = 0; x < img.width; ++x) std::swap(row[4 * x], row[4 * x + 2]);
    }
}
