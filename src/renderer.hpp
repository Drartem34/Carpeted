// OpenGL ES 3 renderer: shaded floor band, cards, rug shadow and the rug.
#pragma once
#include <GLES3/gl3.h>

#include <memory>
#include <vector>

#include "cards.hpp"
#include "cloth.hpp"

struct RenderParams {
    int fb_w, fb_h;              // framebuffer, device px
    float view_w, view_h;        // logical px
    float scale;                 // device px per logical px
    bool shade_floor = true;     // dark see-through band behind everything
    const Item* selected = nullptr;
    const Item* carried = nullptr;   // drawn above the rug, lifted
    const Item* hidden = nullptr;    // being dragged out: not drawn
};

class Renderer {
public:
    float focal = 1800;

    bool init(const Cloth& cloth);    // GL context must be current
    void set_cloth(const Cloth& cloth);
    void destroy(std::vector<std::unique_ptr<Item>>& items);
    void forget(Item& item);
    void render(const RenderParams& p, const Cloth& cloth,
                std::vector<std::unique_ptr<Item>>& items);
    std::vector<uint8_t> read_pixels(int w, int h);   // RGBA, top row first

private:
    GLuint quad_prog_ = 0, rug_prog_ = 0, shadow_prog_ = 0;
    GLuint quad_vao_ = 0, quad_vbo_ = 0;
    GLuint rug_vao_ = 0, rug_vbo_ = 0, uv_vbo_ = 0, ibo_ = 0;
    GLuint rug_tex_ = 0;
    int index_count_ = 0;
    std::vector<float> scratch_;
    void build_rug_buffers(const Cloth& cloth);
    void draw_item(Item& it, const RenderParams& p, float lift);
    void quad(GLuint tex, float cx, float cy, float w, float h, float angle, float alpha,
              int mode, const float* color = nullptr);
};
