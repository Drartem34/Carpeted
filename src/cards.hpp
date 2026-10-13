// The things lying on the floor and how each one is drawn.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

enum class Kind { File, Image, Link, Text, Window };

struct Item {
    std::string id;
    Kind kind = Kind::File;
    float x = 0, y = 0, angle = 0;     // centre on the floor, logical px
    float w = 96, h = 100;
    bool automatic = false;            // appeared by itself (download, screenshot)
    double added = 0;                  // unix time
    std::string path;                  // File, Image
    std::string url;                   // Link
    std::string text;                  // Text
    std::string address, wclass, title, thumb;   // Window
    std::string owned;                 // a file only we keep (pasted picture)

    // renderer state
    unsigned texture = 0;
    float texture_scale = 0;

    std::string label() const;
    bool contains(float px, float py) const;
    bool exists() const;
    Json to_json() const;
    static bool from_json(const Json& j, Item& out);
    void set_size();
};

const char* kind_name(Kind k);

// Card pixels: cairo ARGB32, i.e. premultiplied BGRA bytes in memory
// (exactly what wl_shm ARGB8888 wants).
struct CardImage {
    int width = 0, height = 0, stride = 0;
    std::vector<uint8_t> bgra;
};

CardImage render_card(const Item& item, float scale);
// premultiplied BGRA -> premultiplied RGBA, in place
void bgra_to_rgba(CardImage& img);
