#include "renderer.hpp"

#include <cmath>
#include <cstdio>

#include "rugtex.hpp"

namespace {

const char* PROJECT = R"(
uniform vec2 uView;
uniform float uFocal;
vec2 projectXY(vec3 p) {
    vec2 c = uView * 0.5;
    float s = uFocal / max(uFocal - p.z, 1.0);
    return c + (p.xy - c) * s;
}
vec4 toClip(vec2 sp, float depth) {
    return vec4(sp.x / uView.x * 2.0 - 1.0, 1.0 - sp.y / uView.y * 2.0, depth, 1.0);
}
)";

const char* QUAD_VS = R"(
layout(location = 0) in vec2 aCorner;
uniform vec4 uRect;        // centre x, y and w, h (logical px)
uniform float uAngle;
out vec2 vUv;
void main() {
    vUv = aCorner;
    vec2 p = (aCorner - 0.5) * uRect.zw;
    float c = cos(uAngle), s = sin(uAngle);
    p = vec2(c * p.x - s * p.y, s * p.x + c * p.y);
    gl_Position = toClip(uRect.xy + p, 0.0);
}
)";

const char* QUAD_FS = R"(
precision mediump float;
in vec2 vUv;
uniform sampler2D uTex;
uniform vec4 uColor;       // premultiplied
uniform int uMode;         // 0 texture, 1 solid colour, 2 floor shade
uniform float uAlpha;
out vec4 fragColor;
void main() {
    vec4 c;
    if (uMode == 0) c = texture(uTex, vUv);
    else if (uMode == 1) c = uColor;
    else {
        float t = vUv.y;
        c = vec4(0.0, 0.0, 0.0, (0.18 + 0.5 * pow(t, 1.6)) * clamp(t * 6.0, 0.0, 1.0));
    }
    fragColor = c * uAlpha;
}
)";

const char* RUG_VS = R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
out vec2 vUv;
out vec3 vNormal;
void main() {
    vUv = aUv;
    vNormal = aNormal;
    gl_Position = toClip(projectXY(aPos), -aPos.z / 2000.0);
}
)";

const char* RUG_FS = R"(
precision highp float;
in vec2 vUv;
in vec3 vNormal;
uniform sampler2D uTex;
uniform vec3 uLight;
out vec4 fragColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 col = texture(uTex, vUv).rgb;
    if (n.z < 0.0) {
        // back side: the same weave seen from behind, duller and darker
        n = -n;
        float g = dot(col, vec3(0.3, 0.59, 0.11));
        col = mix(vec3(g), col, 0.55) * 0.62;
    }
    // exaggerate slopes a little so small bumps read well from above
    n = normalize(vec3(n.xy * 2.5, n.z));
    float diffuse = max(dot(n, uLight), 0.0);
    float light = 0.28 + 0.82 * diffuse + 0.08 * pow(1.0 - n.z, 2.0) * diffuse;
    col *= light;
    vec2 e = min(vUv, 1.0 - vUv) / max(fwidth(vUv), vec2(1e-6));
    float a = clamp(min(e.x, e.y) + 0.25, 0.0, 1.0);    // anti-aliased edges
    fragColor = vec4(col * a, a);
}
)";

// A card lying on the rug: a small mesh draped over the cloth surface.
const char* CARD_VS = R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in float aFacing;   // surface normal z under this vertex
out vec2 vUv;
out float vVis;
void main() {
    vUv = aUv;
    // parts lying on a steep or turned-over bit of rug are not seen from above
    vVis = smoothstep(0.25, 0.55, aFacing);
    gl_Position = toClip(projectXY(aPos), -aPos.z / 2000.0);
}
)";

const char* CARD_FS = R"(
precision mediump float;
in vec2 vUv;
in float vVis;
uniform sampler2D uTex;
uniform vec4 uTint;        // premultiplied highlight, a = strength
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUv) * vVis;
    if (c.a < 0.04) discard;            // do not hide what lies under the empty parts
    fragColor = vec4(mix(c.rgb, uTint.rgb * c.a, uTint.a), c.a);
}
)";

const char* SHADOW_VS = R"(
layout(location = 0) in vec3 aPos;
uniform vec2 uShadowDir;
out float vAlpha;
void main() {
    float h = aPos.z + 2.0;                     // + rug thickness: contact shadow
    vec3 ground = vec3(aPos.xy + uShadowDir * h, 0.0);
    vAlpha = clamp(0.22 + aPos.z / 120.0, 0.0, 0.5);
    gl_Position = toClip(projectXY(ground), 0.8);
}
)";

const char* SHADOW_FS = R"(
precision mediump float;
in float vAlpha;
out vec4 fragColor;
void main() { fragColor = vec4(0.0, 0.0, 0.0, vAlpha); }
)";

GLuint compile(const char* vs_body, const char* fs_body) {
    auto shader = [](GLenum kind, const std::string& src) -> GLuint {
        GLuint sh = glCreateShader(kind);
        const char* s = src.c_str();
        glShaderSource(sh, 1, &s, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            std::fprintf(stderr, "carpeted: shader error: %s\n", log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };
    std::string head = "#version 300 es\nprecision highp float;\n";
    GLuint vs = shader(GL_VERTEX_SHADER, head + PROJECT + vs_body);
    GLuint fs = shader(GL_FRAGMENT_SHADER, "#version 300 es\n" + std::string(fs_body));
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        std::fprintf(stderr, "carpeted: link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint upload(const uint8_t* rgba, int w, int h, bool mipmap) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    return tex;
}

}  // namespace

bool Renderer::init(const Cloth& cloth) {
    quad_prog_ = compile(QUAD_VS, QUAD_FS);
    rug_prog_ = compile(RUG_VS, RUG_FS);
    shadow_prog_ = compile(SHADOW_VS, SHADOW_FS);
    card_prog_ = compile(CARD_VS, CARD_FS);
    if (!quad_prog_ || !rug_prog_ || !shadow_prog_ || !card_prog_) return false;

    // one 5x5 grid mesh, refilled for every card on the rug
    glGenVertexArrays(1, &card_vao_);
    glBindVertexArray(card_vao_);
    glGenBuffers(1, &card_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, card_vbo_);
    glBufferData(GL_ARRAY_BUFFER, 25 * 6 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(5 * sizeof(float)));
    std::vector<uint16_t> idx;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            uint16_t a = i * 5 + j, b = a + 1, c = a + 5, d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    glGenBuffers(1, &card_ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, card_ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint16_t), idx.data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);

    const float corners[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenVertexArrays(1, &quad_vao_);
    glBindVertexArray(quad_vao_);
    glGenBuffers(1, &quad_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    build_rug_buffers(cloth);

    int w, h;
    std::vector<uint8_t> rug = generate_rug(w, h);
    rug_tex_ = upload(rug.data(), w, h, true);
    return true;
}

void Renderer::build_rug_buffers(const Cloth& c) {
    glGenVertexArrays(1, &rug_vao_);
    glBindVertexArray(rug_vao_);
    glGenBuffers(1, &rug_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, rug_vbo_);
    glBufferData(GL_ARRAY_BUFFER, c.count() * 6 * sizeof(float), nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glGenBuffers(1, &uv_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, uv_vbo_);
    glBufferData(GL_ARRAY_BUFFER, c.uv.size() * sizeof(float), c.uv.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, c.triangles.size() * sizeof(uint16_t),
                 c.triangles.data(), GL_STATIC_DRAW);
    index_count_ = int(c.triangles.size());
    glBindVertexArray(0);
}

void Renderer::set_cloth(const Cloth& c) {
    GLuint bufs[] = {rug_vbo_, uv_vbo_, ibo_};
    glDeleteBuffers(3, bufs);
    glDeleteVertexArrays(1, &rug_vao_);
    build_rug_buffers(c);
}

void Renderer::forget(Item& it) {
    if (it.texture) glDeleteTextures(1, &it.texture);
    it.texture = 0;
    it.texture_scale = 0;
}

void Renderer::destroy(std::vector<std::unique_ptr<Item>>& items) {
    for (auto& it : items) forget(*it);
    GLuint bufs[] = {quad_vbo_, rug_vbo_, uv_vbo_, ibo_, card_vbo_, card_ibo_};
    glDeleteBuffers(6, bufs);
    GLuint vaos[] = {quad_vao_, rug_vao_, card_vao_};
    glDeleteVertexArrays(3, vaos);
    glDeleteProgram(card_prog_);
    glDeleteTextures(1, &rug_tex_);
    glDeleteProgram(quad_prog_);
    glDeleteProgram(rug_prog_);
    glDeleteProgram(shadow_prog_);
    *this = Renderer{};
}

void Renderer::quad(GLuint tex, float cx, float cy, float w, float h, float angle, float alpha,
                    int mode, const float* color) {
    GLuint p = quad_prog_;
    glUniform4f(glGetUniformLocation(p, "uRect"), cx, cy, w, h);
    glUniform1f(glGetUniformLocation(p, "uAngle"), angle);
    glUniform1f(glGetUniformLocation(p, "uAlpha"), alpha);
    glUniform1i(glGetUniformLocation(p, "uMode"), mode);
    if (color) glUniform4fv(glGetUniformLocation(p, "uColor"), 1, color);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::ensure_texture(Item& it, const RenderParams& p) {
    if (!it.texture || it.texture_scale != p.scale) {
        CardImage img = render_card(it, p.scale);
        bgra_to_rgba(img);
        if (it.texture) glDeleteTextures(1, &it.texture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, img.stride / 4);
        it.texture = upload(img.bgra.data(), img.width, img.height, false);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        it.texture_scale = p.scale;
    }
}

void Renderer::draw_rug_item(Item& it, const RenderParams& p, const Cloth& c, float bias) {
    float verts[25 * 6];
    float ca = std::cos(it.angle), sa = std::sin(it.angle);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            float lx = (j / 4.f - 0.5f) * it.w, ly = (i / 4.f - 0.5f) * it.h;
            V3 pos, n;
            c.sample(it.u + ca * lx - sa * ly, it.v + sa * lx + ca * ly, pos, n);
            // still inside the roll: wait until this part of the rug is out
            if (p.animating && (pos.z > 1.5f || n.z < 0.9f)) return;
            float* v = &verts[(i * 5 + j) * 6];
            v[0] = pos.x + n.x * bias;
            v[1] = pos.y + n.y * bias;
            v[2] = pos.z + n.z * bias;
            v[3] = j / 4.f;
            v[4] = i / 4.f;
            v[5] = n.z;
        }
    ensure_texture(it, p);
    glBindBuffer(GL_ARRAY_BUFFER, card_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof verts, verts);
    float sel = &it == p.selected ? 0.3f : 0.f;
    glUniform4f(glGetUniformLocation(card_prog_, "uTint"), 0.3f, 0.5f, 1.f, sel);
    glBindTexture(GL_TEXTURE_2D, it.texture);
    glDrawElements(GL_TRIANGLES, 96, GL_UNSIGNED_SHORT, nullptr);
}

void Renderer::draw_item(Item& it, const RenderParams& p, float lift) {
    ensure_texture(it, p);
    float x = it.x, y = it.y;
    if (&it == p.selected) {
        const float c[] = {0.2f * 0.4f, 0.35f * 0.4f, 0.65f * 0.4f, 0.4f};
        quad(0, x, y, it.w + 4, it.h + 4, it.angle, 1, 1, c);
    }
    if (lift > 0) {
        quad(it.texture, x + lift * 0.5f, y + lift * 0.7f, it.w, it.h, it.angle, 0.35f, 0);
        x -= 2;
        y -= 3;
    }
    quad(it.texture, x, y, it.w, it.h, it.angle, 1, 0);
}

void Renderer::render(const RenderParams& p, const Cloth& c,
                      std::vector<std::unique_ptr<Item>>& items) {
    glViewport(0, 0, p.fb_w, p.fb_h);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);

    auto common = [&](GLuint prog) {
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "uView"), p.view_w, p.view_h);
        glUniform1f(glGetUniformLocation(prog, "uFocal"), focal);
    };

    // floor band and the cards lying on it
    common(quad_prog_);
    glUniform1i(glGetUniformLocation(quad_prog_, "uTex"), 0);
    glBindVertexArray(quad_vao_);
    if (p.shade_floor) quad(0, p.view_w / 2, p.view_h / 2, p.view_w, p.view_h, 0, 1, 2);
    for (auto& it : items)
        if (!it->on_rug && it.get() != p.carried && it.get() != p.hidden) draw_item(*it, p, 0);

    // the rug: shadow first, then the cloth with depth testing
    scratch_.resize(size_t(c.count()) * 6);
    for (int k = 0; k < c.count(); ++k) {
        float* v = &scratch_[size_t(k) * 6];
        v[0] = c.pos[k].x; v[1] = c.pos[k].y; v[2] = c.pos[k].z;
        v[3] = c.normals[k].x; v[4] = c.normals[k].y; v[5] = c.normals[k].z;
    }
    glBindVertexArray(rug_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, rug_vbo_);
    glBufferData(GL_ARRAY_BUFFER, scratch_.size() * sizeof(float), scratch_.data(),
                 GL_STREAM_DRAW);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    common(shadow_prog_);
    glUniform2f(glGetUniformLocation(shadow_prog_, "uShadowDir"), 0.25f, 0.35f);
    glDrawElements(GL_TRIANGLES, index_count_, GL_UNSIGNED_SHORT, nullptr);

    common(rug_prog_);
    float l[] = {-0.35f, -0.45f, 0.82f}, n = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    glUniform3f(glGetUniformLocation(rug_prog_, "uLight"), l[0] / n, l[1] / n, l[2] / n);
    glUniform1i(glGetUniformLocation(rug_prog_, "uTex"), 0);
    glBindTexture(GL_TEXTURE_2D, rug_tex_);
    glDrawElements(GL_TRIANGLES, index_count_, GL_UNSIGNED_SHORT, nullptr);

    // cards lying on the rug, depth tested so a folded flap covers them
    common(card_prog_);
    glUniform1i(glGetUniformLocation(card_prog_, "uTex"), 0);
    glBindVertexArray(card_vao_);
    float bias = 0.8f;
    for (auto& it : items) {
        if (!it->on_rug || it.get() == p.carried || it.get() == p.hidden) continue;
        draw_rug_item(*it, p, c, bias);
        bias += 0.03f;          // later cards lie on top of earlier ones
    }
    glDisable(GL_DEPTH_TEST);

    // a window card being carried floats above everything
    if (p.carried) {
        common(quad_prog_);
        glBindVertexArray(quad_vao_);
        draw_item(*const_cast<Item*>(p.carried), p, 10);
    }
    glBindVertexArray(0);
}

std::vector<uint8_t> Renderer::read_pixels(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4), out(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; ++y)
        std::copy(px.begin() + size_t(h - 1 - y) * w * 4, px.begin() + size_t(h - y) * w * 4,
                  out.begin() + size_t(y) * w * 4);
    return out;
}
