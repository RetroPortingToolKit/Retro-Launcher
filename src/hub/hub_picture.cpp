#include "hub/hub_picture.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace retcomm::hub {

namespace {

// GL 3.0 entry points: the hub's 3.2 core context has them, but opengl32.dll
// exports none of them and SDL_opengl.h declares no prototypes, so they are
// looked up once, with the context current. Without the blit, sharp-bilinear
// draws as bilinear; without mipmaps, a minified frame is plain GL_LINEAR.
struct Gl3 {
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;
    PFNGLGENERATEMIPMAPPROC GenerateMipmap = nullptr;
    bool blit = false;
};

const Gl3& gl3() {
    static const Gl3 g = [] {
        Gl3 f;
        auto get = [](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_GL_GetProcAddress(name));
        };
        get(f.GenFramebuffers, "glGenFramebuffers");
        get(f.DeleteFramebuffers, "glDeleteFramebuffers");
        get(f.BindFramebuffer, "glBindFramebuffer");
        get(f.FramebufferTexture2D, "glFramebufferTexture2D");
        get(f.CheckFramebufferStatus, "glCheckFramebufferStatus");
        get(f.BlitFramebuffer, "glBlitFramebuffer");
        get(f.GenerateMipmap, "glGenerateMipmap");
        f.blit = f.GenFramebuffers && f.DeleteFramebuffers && f.BindFramebuffer &&
                 f.FramebufferTexture2D && f.CheckFramebufferStatus && f.BlitFramebuffer;
        if (!f.blit) SDL_Log("retro-hub: no framebuffer blit; sharp-bilinear draws as bilinear");
        if (!f.GenerateMipmap) SDL_Log("retro-hub: no glGenerateMipmap; a minified frame is GL_LINEAR");
        return f;
    }();
    return g;
}

// The whole multiple of `size` that `px` holds, at least 1. A hair of slack:
// a window exactly 3x the frame must not come out as 2.999.
std::uint32_t whole_multiple(float px, std::uint32_t size) {
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(px / float(size) + 1e-3f));
}

} // namespace

PictureRect fit_picture(ImVec2 disp, ImVec2 fb_scale, std::uint32_t tex_w, std::uint32_t tex_h,
                        std::uint32_t aspect_num, std::uint32_t aspect_den, bool integer_scale) {
    PictureRect r;
    if (!tex_w || !tex_h) return r;
    // The hub's letterbox, exactly as it always was: the default draws the
    // same pixels it always did.
    const float aspect = (aspect_num && aspect_den) ? float(aspect_num) / float(aspect_den)
                                                    : float(tex_w) / float(tex_h);
    float w = disp.x, h = disp.x / aspect;
    if (h > disp.y) {
        h = disp.y;
        w = disp.y * aspect;
    }
    const float sx = fb_scale.x > 0.f ? fb_scale.x : 1.f;
    const float sy = fb_scale.y > 0.f ? fb_scale.y : 1.f;
    if (integer_scale) {
        // In framebuffer pixels, where "whole" means something.
        const float n = std::floor(h * sy / float(tex_h) + 1e-3f);
        if (n >= 1.f) {
            const float ph = n * float(tex_h);
            const float pw = std::round(ph * aspect * sx / sy);
            r.w = pw / sx;
            r.h = ph / sy;
            r.x = std::floor((disp.x * sx - pw) * 0.5f) / sx;
            r.y = std::floor((disp.y * sy - ph) * 0.5f) / sy;
            return r;
        }
    }
    r.x = (disp.x - w) * 0.5f;
    r.y = (disp.y - h) * 0.5f;
    r.w = w;
    r.h = h;
    return r;
}

void Picture::upload(const void* rgba, std::uint32_t w, std::uint32_t h, std::uint32_t aspect_num,
                     std::uint32_t aspect_den) {
    if (!tex_) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        mag_ = GL_LINEAR;
        mipmapped_ = false;
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != w_ || h != h_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(w), GLsizei(h), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba);
        w_ = w;
        h_ = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba);
    }
    aspect_num_ = aspect_num;
    aspect_den_ = aspect_den;
    ++rev_;
    // The chain follows the frame (and a resized level 0 leaves it incomplete).
    if (mipmapped_) gl3().GenerateMipmap(GL_TEXTURE_2D);
}

void Picture::set_minified(bool on) {
    if (on == mipmapped_ || (on && !gl3().GenerateMipmap)) return;
    glBindTexture(GL_TEXTURE_2D, tex_);
    if (on) gl3().GenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, on ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    mipmapped_ = on;
}

// tex_ blown up kx by ky with nearest, into pre_: redone per new frame or size.
// 0 when it cannot be (the caller draws tex_ instead).
unsigned int Picture::prescaled(std::uint32_t kx, std::uint32_t ky) {
    const Gl3& g = gl3();
    if (!g.blit) return 0;
    const std::uint32_t pw = w_ * kx, ph = h_ * ky;
    if (!pre_) {
        glGenTextures(1, &pre_);
        glBindTexture(GL_TEXTURE_2D, pre_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g.GenFramebuffers(1, &read_fb_);
        g.GenFramebuffers(1, &draw_fb_);
    }
    if (pw != pre_w_ || ph != pre_h_) {
        glBindTexture(GL_TEXTURE_2D, pre_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(pw), GLsizei(ph), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        pre_w_ = pw;
        pre_h_ = ph;
        pre_rev_ = 0; // rev_ is at least 1 once a frame is up
    }
    if (pre_rev_ == rev_) return pre_;

    // A blit bypasses the fragment pipeline but not the scissor test; the
    // bindings go back to what ImGui's frame had.
    GLint was_read = 0, was_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &was_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &was_draw);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb_);
    g.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb_);
    g.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pre_, 0);
    const bool ok = g.CheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                    g.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
        g.BlitFramebuffer(0, 0, GLint(w_), GLint(h_), 0, 0, GLint(pw), GLint(ph),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(was_read));
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(was_draw));
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (!ok) {
        static bool said = false;
        if (!said) SDL_Log("retro-hub: the sharp-bilinear prescale framebuffer is incomplete; "
                           "drawing bilinear");
        said = true;
        return 0;
    }
    pre_rev_ = rev_;
    return pre_;
}

void Picture::draw(ImDrawList* dl, ImU32 tint) {
    if (!tex_ || !w_ || !h_) return;
    const ImGuiIO& io = ImGui::GetIO();
    const PictureRect r = fit_picture(io.DisplaySize, io.DisplayFramebufferScale, w_, h_,
                                      aspect_num_, aspect_den_, style_.integer_scale);
    // What the frame is scaled by, in framebuffer pixels.
    const float px_w = r.w * (io.DisplayFramebufferScale.x > 0.f ? io.DisplayFramebufferScale.x : 1.f);
    const float px_h = r.h * (io.DisplayFramebufferScale.y > 0.f ? io.DisplayFramebufferScale.y : 1.f);
    set_minified(px_w < float(w_) || px_h < float(h_));
    const int mag = style_.filter == OutputFilter::Nearest ? GL_NEAREST : GL_LINEAR;
    if (mag != mag_) {
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
        mag_ = mag;
    }
    unsigned int tex = tex_;
    if (style_.filter == OutputFilter::SharpBilinear && !mipmapped_) {
        const std::uint32_t kx = whole_multiple(px_w, w_), ky = whole_multiple(px_h, h_);
        if (kx > 1 || ky > 1)
            if (const unsigned int p = prescaled(kx, ky)) tex = p;
    }
    dl->AddImage((ImTextureID)(intptr_t)tex, ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h),
                 ImVec2(0, 0), ImVec2(1, 1), tint);
}

void Picture::release() {
    if (tex_) glDeleteTextures(1, &tex_);
    if (pre_) glDeleteTextures(1, &pre_);
    if (read_fb_ || draw_fb_) {
        const Gl3& g = gl3();
        if (read_fb_) g.DeleteFramebuffers(1, &read_fb_);
        if (draw_fb_) g.DeleteFramebuffers(1, &draw_fb_);
    }
    tex_ = pre_ = read_fb_ = draw_fb_ = 0;
    w_ = h_ = pre_w_ = pre_h_ = 0;
    mag_ = 0;
    mipmapped_ = false;
}

} // namespace retcomm::hub
