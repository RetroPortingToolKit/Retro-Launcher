#pragma once

// The core's frame in the hub's window: one texture, letterboxed to the core's
// stated aspect (or square pixels) and drawn behind everything through ImGui's
// background list. Presentation only: nothing here reaches the core, and the
// frame it is handed is never changed.
//
// How the frame is scaled is the player's (PictureStyle, hub_core_settings.hpp
// "display"; per title, over every title of the platform):
//   bilinear        GL_LINEAR, as the hub always drew it (the default).
//   nearest         GL_NEAREST: hard pixel edges; at a scale that is not a
//                   whole number, rows and columns come out uneven.
//   sharp-bilinear  the frame blown up by the largest whole multiple that fits
//                   (per axis) with nearest, into a second texture by a
//                   framebuffer blit, and that drawn with GL_LINEAR: pixels
//                   stay crisp and even, and only their edges blend, over at
//                   most one screen pixel. A prescale rather than the usual
//                   texel-snapping fragment shader because the picture is an
//                   ImGui draw-list image: the backend's own shader draws it,
//                   and the prescaled texture is just another image to it. A
//                   shader of our own would mean a draw callback swapping the
//                   backend's program and re-deriving its projection. The blit
//                   runs once per new frame (or window size), outside ImGui.
//                   Below 2x on both axes it is plain bilinear.
// A frame drawn smaller than itself (a core rendering above the window's
// resolution: n64lle's raised internal resolution) is minified through a
// mipmap chain, in every mode: GL_LINEAR alone reads 2x2 texels however far
// it shrinks, and aliases. The chain is built only while the frame is
// minified, once per new frame.

#include "hub/hub_core_settings.hpp"

#include "imgui.h"

#include <cstdint>

namespace retcomm::hub {

// A rectangle in ImGui units (DisplaySize's).
struct PictureRect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// Where a tex_w x tex_h frame is drawn on a `disp` display whose framebuffer
// has `fb_scale` pixels per unit (ImGuiIO::DisplayFramebufferScale). Letterboxed
// to aspect_num:aspect_den, or the frame's own shape when either is 0. With
// integer_scale, the height is the largest whole multiple of tex_h that fits
// (in framebuffer pixels) and the width follows the aspect, rounded to a whole
// pixel -- a whole multiple of tex_w too when the pixels are square -- and the
// rectangle sits on whole pixels; a frame too tall for one multiple is fitted
// as without it. Pure, so it is testable without a window.
PictureRect fit_picture(ImVec2 disp, ImVec2 fb_scale, std::uint32_t tex_w, std::uint32_t tex_h,
                        std::uint32_t aspect_num, std::uint32_t aspect_den, bool integer_scale);

class Picture {
public:
    Picture() = default;
    Picture(const Picture&) = delete;
    Picture& operator=(const Picture&) = delete;

    void set_style(const PictureStyle& s) { style_ = s; }
    const PictureStyle& style() const { return style_; }

    // A new frame: w x h RGBA8, rows tightly packed. The GL context must be
    // current.
    void upload(const void* rgba, std::uint32_t w, std::uint32_t h, std::uint32_t aspect_num,
                std::uint32_t aspect_den);
    bool empty() const { return tex_ == 0; }

    // Into `dl` (inside an ImGui frame), letterboxed on the display, tinted.
    void draw(ImDrawList* dl, ImU32 tint);

    // Frees the textures; the GL context must be current.
    void release();

private:
    void set_minified(bool on);
    unsigned int prescaled(std::uint32_t kx, std::uint32_t ky);

    PictureStyle style_;
    unsigned int tex_ = 0;
    std::uint32_t w_ = 0, h_ = 0;
    std::uint32_t aspect_num_ = 0, aspect_den_ = 0;
    std::uint64_t rev_ = 0;        // bumped per uploaded frame
    int mag_ = 0;                  // tex_'s GL_TEXTURE_MAG_FILTER, as last set
    bool mipmapped_ = false;       // tex_ is minified through its mipmap chain

    // sharp-bilinear: tex_ times (kx, ky), and the framebuffers that blit it.
    unsigned int pre_ = 0, read_fb_ = 0, draw_fb_ = 0;
    std::uint32_t pre_w_ = 0, pre_h_ = 0;
    std::uint64_t pre_rev_ = 0;
};

} // namespace retcomm::hub
