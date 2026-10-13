"""OpenGL renderer (core profile 3.3) used inside a Gtk.GLArea."""


import numpy as np
from OpenGL import GL

PROJECT = """
uniform vec2 uView;
uniform float uFocal;
vec2 projectXY(vec3 p) {
    vec2 c = uView * 0.5;
    float s = uFocal / max(uFocal - p.z, 1.0);
    return c + (p.xy - c) * s;
}
vec4 toClip(vec2 sp, float depth) {
    return vec4(sp.x / uView.x * 2.0 - 1.0, 1.0 - sp.y / uView.y * 2.0,
                depth, 1.0);
}
"""

QUAD_VS = """#version 330 core
layout(location = 0) in vec2 aCorner;
uniform vec4 uRect;          // x, y, w, h in logical pixels
""" + PROJECT + """
out vec2 vUv;
void main() {
    vUv = aCorner;
    gl_Position = toClip(uRect.xy + aCorner * uRect.zw, 0.0);
}
"""

QUAD_FS = """#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform vec4 uColor;         // premultiplied; used when uSolid = 1
uniform int uSolid;
uniform float uAlpha;
out vec4 fragColor;
void main() {
    vec4 c = uSolid == 1 ? uColor : texture(uTex, vUv);
    fragColor = c * uAlpha;
}
"""

RUG_VS = """#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec3 aNormal;
""" + PROJECT + """
out vec2 vUv;
out vec3 vNormal;
out float vHeight;
void main() {
    vUv = aUv;
    vNormal = aNormal;
    vHeight = aPos.z;
    gl_Position = toClip(projectXY(aPos), -aPos.z / 2000.0);
}
"""

RUG_FS = """#version 330 core
in vec2 vUv;
in vec3 vNormal;
in float vHeight;
uniform sampler2D uTex;
uniform vec3 uLight;
out vec4 fragColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 col = texture(uTex, vUv).rgb;
    if (n.z < 0.0) {
        // Back side: the same weave seen from behind, duller and darker.
        n = -n;
        float g = dot(col, vec3(0.3, 0.59, 0.11));
        col = mix(vec3(g), col, 0.55) * 0.62;
    }
    // exaggerate slopes a little so small bumps read well from above
    n = normalize(vec3(n.xy * 2.5, n.z));
    float diffuse = max(dot(n, uLight), 0.0);
    float light = 0.28 + 0.82 * diffuse;
    // a bit of sheen on wool facing the light at grazing angles
    light += 0.08 * pow(1.0 - n.z, 2.0) * diffuse;
    col *= light;
    // anti-aliased rug edges
    vec2 e = min(vUv, 1.0 - vUv) / max(fwidth(vUv), vec2(1e-6));
    float a = clamp(min(e.x, e.y) + 0.25, 0.0, 1.0);
    fragColor = vec4(col * a, a);
}
"""

SHADOW_VS = """#version 330 core
layout(location = 0) in vec3 aPos;
""" + PROJECT + """
uniform vec2 uShadowDir;
out float vAlpha;
void main() {
    float h = aPos.z + 2.0;          // + rug thickness: contact shadow
    vec3 ground = vec3(aPos.xy + uShadowDir * h, 0.0);
    vAlpha = clamp(0.22 + aPos.z / 120.0, 0.0, 0.5);
    gl_Position = toClip(projectXY(ground), 0.8);
}
"""

SHADOW_FS = """#version 330 core
in float vAlpha;
out vec4 fragColor;
void main() { fragColor = vec4(0.0, 0.0, 0.0, vAlpha); }
"""


def _compile(vs, fs):
    def shader(kind, src):
        sh = GL.glCreateShader(kind)
        GL.glShaderSource(sh, src)
        GL.glCompileShader(sh)
        if not GL.glGetShaderiv(sh, GL.GL_COMPILE_STATUS):
            raise RuntimeError(GL.glGetShaderInfoLog(sh).decode())
        return sh

    prog = GL.glCreateProgram()
    shaders = [shader(GL.GL_VERTEX_SHADER, vs), shader(GL.GL_FRAGMENT_SHADER, fs)]
    for sh in shaders:
        GL.glAttachShader(prog, sh)
    GL.glLinkProgram(prog)
    if not GL.glGetProgramiv(prog, GL.GL_LINK_STATUS):
        raise RuntimeError(GL.glGetProgramInfoLog(prog).decode())
    for sh in shaders:
        GL.glDeleteShader(sh)
    return prog


def premultiply(rgba):
    out = rgba.astype(np.float32)
    out[..., :3] *= out[..., 3:4] / 255.0
    return out.astype(np.uint8)


def upload_texture(rgba, mipmap=False, texture=None):
    rgba = np.ascontiguousarray(rgba, dtype=np.uint8)
    h, w = rgba.shape[:2]
    tex = texture if texture is not None else GL.glGenTextures(1)
    GL.glBindTexture(GL.GL_TEXTURE_2D, tex)
    GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)
    GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8, w, h, 0, GL.GL_RGBA,
                    GL.GL_UNSIGNED_BYTE, rgba)
    GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S, GL.GL_CLAMP_TO_EDGE)
    GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T, GL.GL_CLAMP_TO_EDGE)
    GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
    if mipmap:
        GL.glGenerateMipmap(GL.GL_TEXTURE_2D)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER,
                           GL.GL_LINEAR_MIPMAP_LINEAR)
    else:
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
    return tex


class Renderer:
    def __init__(self, cloth, rug_rgba, focal=1800.0):
        self.cloth = cloth
        self.focal = focal
        self.rug_rgba = rug_rgba
        self.floor_rgba = None
        self.floor_tex = None
        self.light = np.array([-0.35, -0.45, 0.82])
        self.light /= np.linalg.norm(self.light)
        self.shadow_dir = (0.25, 0.35)

    # ------------------------------------------------------------------ setup
    def realize(self):
        self.quad_prog = _compile(QUAD_VS, QUAD_FS)
        self.rug_prog = _compile(RUG_VS, RUG_FS)
        self.shadow_prog = _compile(SHADOW_VS, SHADOW_FS)

        self.quad_vao = GL.glGenVertexArrays(1)
        GL.glBindVertexArray(self.quad_vao)
        self.quad_vbo = GL.glGenBuffers(1)
        corners = np.array([0, 0, 1, 0, 0, 1, 1, 1], dtype=np.float32)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self.quad_vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, corners.nbytes, corners,
                        GL.GL_STATIC_DRAW)
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(0, 2, GL.GL_FLOAT, False, 0, None)

        c = self.cloth
        self.rug_vao = GL.glGenVertexArrays(1)
        GL.glBindVertexArray(self.rug_vao)
        self.pos_vbo, self.uv_vbo, self.normal_vbo, self.ebo = GL.glGenBuffers(4)
        for vbo, loc, size, data in ((self.pos_vbo, 0, 3, None),
                                     (self.uv_vbo, 1, 2, c.uv),
                                     (self.normal_vbo, 2, 3, None)):
            GL.glBindBuffer(GL.GL_ARRAY_BUFFER, vbo)
            if data is None:
                GL.glBufferData(GL.GL_ARRAY_BUFFER, c.count * size * 4, None,
                                GL.GL_DYNAMIC_DRAW)
            else:
                GL.glBufferData(GL.GL_ARRAY_BUFFER, data.nbytes, data,
                                GL.GL_STATIC_DRAW)
            GL.glEnableVertexAttribArray(loc)
            GL.glVertexAttribPointer(loc, size, GL.GL_FLOAT, False, 0, None)
        GL.glBindBuffer(GL.GL_ELEMENT_ARRAY_BUFFER, self.ebo)
        GL.glBufferData(GL.GL_ELEMENT_ARRAY_BUFFER, c.triangles.nbytes,
                        c.triangles, GL.GL_STATIC_DRAW)
        GL.glBindVertexArray(0)

        self.rug_tex = upload_texture(self.rug_rgba, mipmap=True)
        try:  # anisotropic filtering, if the driver has it
            GL.glTexParameterf(GL.GL_TEXTURE_2D, 0x84FE, 4.0)
        except GL.GLError:
            pass
        self.white_tex = upload_texture(np.full((1, 1, 4), 255, np.uint8))
        if self.floor_rgba is not None:
            self.floor_tex = upload_texture(self.floor_rgba)

    def set_floor(self, rgba):
        """Called from the GL context (or before realize)."""
        self.floor_rgba = rgba
        if hasattr(self, "quad_prog"):
            if rgba is None:
                self.floor_tex = None
            else:
                self.floor_tex = upload_texture(rgba, texture=self.floor_tex)

    # ------------------------------------------------------------- utilities
    def _common_uniforms(self, prog, view):
        GL.glUseProgram(prog)
        GL.glUniform2f(GL.glGetUniformLocation(prog, "uView"), *view)
        GL.glUniform1f(GL.glGetUniformLocation(prog, "uFocal"), self.focal)

    def _quad(self, tex, x, y, w, h, alpha=1.0, color=None):
        p = self.quad_prog
        GL.glUniform4f(GL.glGetUniformLocation(p, "uRect"), x, y, w, h)
        GL.glUniform1f(GL.glGetUniformLocation(p, "uAlpha"), alpha)
        GL.glUniform1i(GL.glGetUniformLocation(p, "uSolid"), color is not None)
        if color is not None:
            GL.glUniform4f(GL.glGetUniformLocation(p, "uColor"), *color)
        GL.glBindTexture(GL.GL_TEXTURE_2D, tex if tex else self.white_tex)
        GL.glDrawArrays(GL.GL_TRIANGLE_STRIP, 0, 4)

    def ensure_item_texture(self, item):
        if item.rgba is not None:
            item.texture = upload_texture(premultiply(item.rgba),
                                          texture=item.texture)
            item.rgba = None

    def forget_items(self, items):
        textures = [it.texture for it in items if it.texture]
        if textures:
            GL.glDeleteTextures(textures)
        for it in items:
            it.texture = None

    # ----------------------------------------------------------------- frame
    def render(self, view, items, selected=None, carried=None, transparent=False):
        from .desktop import TILE_W, TILE_H

        GL.glClearColor(0.0, 0.0, 0.0, 0.0 if transparent else 1.0)
        GL.glClearDepth(1.0)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT | GL.GL_DEPTH_BUFFER_BIT)
        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_ONE, GL.GL_ONE_MINUS_SRC_ALPHA)
        GL.glDisable(GL.GL_DEPTH_TEST)
        GL.glActiveTexture(GL.GL_TEXTURE0)

        # floor and the icons lying on it
        self._common_uniforms(self.quad_prog, view)
        GL.glUniform1i(GL.glGetUniformLocation(self.quad_prog, "uTex"), 0)
        GL.glBindVertexArray(self.quad_vao)
        if self.floor_tex:
            self._quad(self.floor_tex, 0, 0, view[0], view[1])

        def draw_item(it, lift=0.0):
            self.ensure_item_texture(it)
            x, y = it.x - TILE_W / 2, it.y - TILE_H / 2
            if it is selected:
                self._quad(None, x + 4, y, TILE_W - 8, TILE_H,
                           color=(0.16, 0.24, 0.4, 0.35))
            if lift:
                self._quad(it.texture, x + lift * 0.5, y + lift * 0.7,
                           TILE_W, TILE_H, alpha=0.35)
                x, y = x - 2, y - 3
            self._quad(it.texture, x, y, TILE_W, TILE_H)

        for it in items:
            if it is not carried:
                draw_item(it)

        # rug: shadow first, then the cloth itself with depth testing
        c = self.cloth
        normals = c.normals
        GL.glBindVertexArray(self.rug_vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self.pos_vbo)
        GL.glBufferSubData(GL.GL_ARRAY_BUFFER, 0, c.count * 12,
                           c.pos.astype(np.float32))
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self.normal_vbo)
        GL.glBufferSubData(GL.GL_ARRAY_BUFFER, 0, c.count * 12,
                           normals.astype(np.float32))

        GL.glEnable(GL.GL_DEPTH_TEST)
        GL.glDepthFunc(GL.GL_LESS)
        GL.glDepthMask(GL.GL_TRUE)
        self._common_uniforms(self.shadow_prog, view)
        GL.glUniform2f(GL.glGetUniformLocation(self.shadow_prog, "uShadowDir"),
                       *self.shadow_dir)
        GL.glDrawElements(GL.GL_TRIANGLES, c.triangles.size, GL.GL_UNSIGNED_INT,
                          None)

        self._common_uniforms(self.rug_prog, view)
        GL.glUniform3f(GL.glGetUniformLocation(self.rug_prog, "uLight"),
                       *self.light)
        GL.glUniform1i(GL.glGetUniformLocation(self.rug_prog, "uTex"), 0)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self.rug_tex)
        GL.glDrawElements(GL.GL_TRIANGLES, c.triangles.size, GL.GL_UNSIGNED_INT,
                          None)
        GL.glDisable(GL.GL_DEPTH_TEST)

        # an icon being carried floats above everything
        if carried is not None:
            self._common_uniforms(self.quad_prog, view)
            GL.glBindVertexArray(self.quad_vao)
            draw_item(carried, lift=10.0)
        GL.glBindVertexArray(0)

    def read_pixels(self, width, height):
        GL.glPixelStorei(GL.GL_PACK_ALIGNMENT, 1)
        data = GL.glReadPixels(0, 0, width, height, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE)
        img = np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4)
        return img[::-1].copy()


def project(points, view, focal):
    """Numpy twin of projectXY() in the shaders."""
    c = np.array(view, dtype=float) * 0.5
    s = focal / np.maximum(focal - points[..., 2], 1.0)
    return c + (points[..., :2] - c) * s[..., None]


def unproject(sx, sy, z, view, focal):
    """Floor-space xy that projects to screen (sx, sy) at height z."""
    c = np.array(view, dtype=float) * 0.5
    s = focal / max(focal - z, 1.0)
    return c + (np.array([sx, sy]) - c) / s
