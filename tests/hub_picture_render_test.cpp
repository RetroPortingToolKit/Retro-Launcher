// hub_picture: the picture's scaling, rendered for real. A hidden window's GL
// 3.2 core context (as the hub makes), Dear ImGui's OpenGL3 backend, and
// Picture drawing a known frame through the background draw list into an
// offscreen framebuffer; the pixels are read back, checked, and written as
// PNGs to look at. Run as `retro-hub-picture-test <out dir>`; exits 77
// (skipped) when no GL 3.2 context can be had (a headless runner).
//
// Checked:
//   - bilinear draws byte for byte what the hub drew before the setting
//     existed (that code, reproduced in draw_before() below), magnified;
//   - nearest at a whole multiple is exact pixel replication, and
//     sharp-bilinear there is the same picture;
//   - integer scale puts the frame at a whole multiple, centred on whole pixels;
//   - fit_picture's arithmetic, without a window.
// Not checked, only written: how each filter looks at a scale that is not a
// whole number, and a frame larger than the window (mipmapped) against the
// old GL_LINEAR minification.

#include "hub/hub_picture.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace retcomm::hub;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

PFNGLGENFRAMEBUFFERSPROC pGenFramebuffers;
PFNGLBINDFRAMEBUFFERPROC pBindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC pFramebufferTexture2D;

struct Frame {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
};

// A frame with detail at one pixel: a yellow border (so the picture's extent
// can be found), a one-pixel checkerboard on the left, two-pixel red / blue
// columns on the right, and a green diagonal.
Frame test_frame(std::uint32_t w, std::uint32_t h) {
    Frame f{w, h, std::vector<std::uint8_t>(std::size_t(w) * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = &f.rgba[(std::size_t(y) * w + x) * 4];
            std::uint8_t r, g, b;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) r = 255, g = 220, b = 0;
            else if (x * h == y * w || (x * h > y * w && x * h - y * w < h)) r = 0, g = 200, b = 0;
            else if (x < w / 2) r = g = b = ((x + y) & 1) ? 255 : 0;
            else if ((x / 2) & 1) r = 30, g = 60, b = 230;
            else r = 230, g = 40, b = 40;
            p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
        }
    return f;
}

// Frame `f` blown up k times by pixel replication: what nearest must draw.
std::vector<std::uint8_t> replicate(const Frame& f, std::uint32_t k) {
    std::vector<std::uint8_t> out(std::size_t(f.w) * k * f.h * k * 4);
    for (std::uint32_t y = 0; y < f.h * k; ++y)
        for (std::uint32_t x = 0; x < f.w * k; ++x)
            std::memcpy(&out[(std::size_t(y) * f.w * k + x) * 4],
                        &f.rgba[(std::size_t(y / k) * f.w + x / k) * 4], 4);
    return out;
}

struct Target {
    int w = 0, h = 0;
    GLuint fbo = 0, tex = 0;
};

Target make_target(int w, int h) {
    Target t{w, h};
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    pGenFramebuffers(1, &t.fbo);
    pBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    return t;
}

// One hub frame into `t`: the black backdrop PlaySession::draw lays first,
// then `draw`, rendered by the backend; the pixels come back top row first.
std::vector<std::uint8_t> render(const Target& t, const std::function<void(ImDrawList*)>& draw) {
    pBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(t.w), float(t.h));
    io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
    io.DeltaTime = 1.f / 60.f;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    draw(bg);
    ImGui::Render();
    pBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w, t.h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    std::vector<std::uint8_t> px(std::size_t(t.w) * t.h * 4), out(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const std::size_t row = std::size_t(t.w) * 4;
    for (int y = 0; y < t.h; ++y)
        std::memcpy(&out[std::size_t(y) * row], &px[std::size_t(t.h - 1 - y) * row], row);
    return out;
}

// hub_play.cpp before the output filter (04bf4a3): upload_frame()'s texture
// and draw()'s letterbox, as they were.
struct Before {
    unsigned int tex = 0;
    std::uint32_t tex_w = 0, tex_h = 0, aspect_num = 0, aspect_den = 0;
    void upload(const Frame& f, std::uint32_t an, std::uint32_t ad) {
        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(f.w), GLsizei(f.h), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, f.rgba.data());
        tex_w = f.w, tex_h = f.h, aspect_num = an, aspect_den = ad;
    }
    void draw(ImDrawList* bg) const {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float aspect = (aspect_num && aspect_den) ? float(aspect_num) / float(aspect_den)
                                                        : float(tex_w) / float(tex_h);
        float w = disp.x, h = disp.x / aspect;
        if (h > disp.y) {
            h = disp.y;
            w = disp.y * aspect;
        }
        const ImVec2 p0((disp.x - w) * 0.5f, (disp.y - h) * 0.5f);
        bg->AddImage((ImTextureID)(intptr_t)tex, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0),
                     ImVec2(1, 1), IM_COL32_WHITE);
    }
};

void save_png(const fs::path& p, int w, int h, const std::vector<std::uint8_t>& px) {
    if (!stbi_write_png(p.string().c_str(), w, h, 4, px.data(), w * 4))
        check(false, "write " + p.string());
}

// The first and last rows and columns holding anything but black.
struct Box {
    int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
};
Box lit_box(const std::vector<std::uint8_t>& px, int w, int h) {
    Box b;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &px[(std::size_t(y) * w + x) * 4];
            if (!p[0] && !p[1] && !p[2]) continue;
            if (b.x0 < 0 || x < b.x0) b.x0 = x;
            if (b.y0 < 0 || y < b.y0) b.y0 = y;
            b.x1 = std::max(b.x1, x);
            b.y1 = std::max(b.y1, y);
        }
    return b;
}

std::size_t differing(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
        n += std::memcmp(&a[i], &b[i], 4) != 0;
    return n;
}

void test_fit() {
    // The default is the hub's old letterbox, to the bit.
    const PictureRect r = fit_picture(ImVec2(1280, 800), ImVec2(1, 1), 640, 480, 4, 3, false);
    check(r.w == 800.f * (4.f / 3.f) && r.h == 800.f && r.y == 0.f &&
              r.x == (1280.f - r.w) * 0.5f,
          "fit: pillarboxed 4:3");
    // Integer: 480 lines into 800 is 1x; 240 into 800 is 3x = 720, centred.
    const PictureRect i = fit_picture(ImVec2(1280, 800), ImVec2(1, 1), 320, 240, 4, 3, true);
    check(i.w == 960.f && i.h == 720.f && i.x == 160.f && i.y == 40.f, "fit: integer 3x");
    // Framebuffer scale 1.5 (a 1920x1200 framebuffer): 5x = 1200 pixels.
    const PictureRect s = fit_picture(ImVec2(1280, 800), ImVec2(1.5f, 1.5f), 320, 240, 4, 3, true);
    check(std::fabs(s.h * 1.5f - 1200.f) < 1e-3f && std::fabs(s.w * 1.5f - 1600.f) < 1e-3f,
          "fit: integer counts framebuffer pixels");
    // A frame taller than the window cannot take a whole multiple: fitted.
    const PictureRect big = fit_picture(ImVec2(640, 480), ImVec2(1, 1), 2560, 1920, 4, 3, true);
    check(big.w == 640.f && big.h == 480.f, "fit: too tall for 1x, fitted as usual");
    // Non-square pixels: whole lines, the width follows the aspect.
    const PictureRect ns = fit_picture(ImVec2(1280, 800), ImVec2(1, 1), 640, 240, 4, 3, true);
    check(ns.h == 720.f && ns.w == 960.f, "fit: 640x240 at 4:3 is 3 lines per line");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
        return 2;
    }
    const fs::path out = argv[1];
    std::error_code ec;
    fs::create_directories(out, ec);
    test_fit();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "skipped: no video (%s)\n", SDL_GetError());
        return g_failures ? 1 : 77;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_Window* win = SDL_CreateWindow("hub_picture", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext gl = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!gl) {
        std::fprintf(stderr, "skipped: no GL 3.2 context (%s)\n", SDL_GetError());
        SDL_Quit();
        return g_failures ? 1 : 77;
    }
    SDL_GL_MakeCurrent(win, gl);
    pGenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(SDL_GL_GetProcAddress("glGenFramebuffers"));
    pBindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(SDL_GL_GetProcAddress("glBindFramebuffer"));
    pFramebufferTexture2D =
        reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(SDL_GL_GetProcAddress("glFramebufferTexture2D"));
    std::printf("GL: %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplOpenGL3_Init("#version 150");

    const PictureStyle bilinear{OutputFilter::Bilinear, false};
    const PictureStyle nearest{OutputFilter::Nearest, false};
    const PictureStyle sharp{OutputFilter::SharpBilinear, false};
    const PictureStyle bilinear_int{OutputFilter::Bilinear, true};
    const PictureStyle sharp_int{OutputFilter::SharpBilinear, true};

    // One case: `f` at an:ad into a w x h window, through each style and the
    // old code; returns the images by name.
    auto run = [&](const std::string& name, const Frame& f, std::uint32_t an, std::uint32_t ad,
                   int w, int h) {
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> imgs;
        const Target t = make_target(w, h);
        Before before;
        before.upload(f, an, ad);
        imgs.emplace_back("before", render(t, [&](ImDrawList* dl) { before.draw(dl); }));
        glDeleteTextures(1, &before.tex);
        const std::pair<const char*, PictureStyle> styles[] = {
            {"bilinear", bilinear},         {"nearest", nearest},     {"sharp-bilinear", sharp},
            {"bilinear+integer", bilinear_int}, {"sharp-bilinear+integer", sharp_int}};
        for (const auto& [label, st] : styles) {
            Picture pic;
            pic.set_style(st);
            pic.upload(f.rgba.data(), f.w, f.h, an, ad);
            // Twice: the second frame reuses the prescale and the mip chain.
            render(t, [&](ImDrawList* dl) { pic.draw(dl, IM_COL32_WHITE); });
            pic.upload(f.rgba.data(), f.w, f.h, an, ad);
            imgs.emplace_back(label, render(t, [&](ImDrawList* dl) { pic.draw(dl, IM_COL32_WHITE); }));
            pic.release();
        }
        for (const auto& [label, px] : imgs) save_png(out / (name + "_" + label + ".png"), w, h, px);
        return imgs;
    };
    auto get = [](const auto& imgs, const std::string& label) -> const std::vector<std::uint8_t>& {
        for (const auto& [l, px] : imgs)
            if (l == label) return px;
        return imgs.front().second;
    };

    // 1. 40x30 at 7.5x (300x225): not a whole multiple.
    const Frame small = test_frame(40, 30);
    const auto a = run("small_7.5x", small, 4, 3, 300, 225);
    check(differing(get(a, "bilinear"), get(a, "before")) == 0,
          "7.5x: bilinear is byte-identical to the hub before the setting");
    const Box ib = lit_box(get(a, "bilinear+integer"), 300, 225);
    check(ib.x0 == 10 && ib.y0 == 7 && ib.x1 == 289 && ib.y1 == 216,
          "7.5x: integer scale is 7x (280x210), centred on whole pixels");
    check(differing(get(a, "sharp-bilinear+integer"), [&] {
              // 7x replicated, placed at (10, 7) on black.
              std::vector<std::uint8_t> want(300 * 225 * 4, 0);
              const auto rep = replicate(small, 7);
              for (int y = 0; y < 225; ++y)
                  for (int x = 0; x < 300; ++x) {
                      std::uint8_t* p = &want[(std::size_t(y) * 300 + x) * 4];
                      p[3] = 255;
                      if (x >= 10 && x < 290 && y >= 7 && y < 217)
                          std::memcpy(p, &rep[(std::size_t(y - 7) * 280 + (x - 10)) * 4], 4);
                  }
              return want;
          }()) == 0,
          "7.5x: sharp-bilinear at integer scale is exact 7x replication");

    // 2. 40x30 at exactly 8x (320x240).
    const auto c = run("small_8x", small, 4, 3, 320, 240);
    check(differing(get(c, "bilinear"), get(c, "before")) == 0, "8x: bilinear byte-identical");
    check(differing(get(c, "nearest"), replicate(small, 8)) == 0, "8x: nearest is replication");
    check(differing(get(c, "sharp-bilinear"), replicate(small, 8)) == 0,
          "8x: sharp-bilinear is replication");

    // 3. n64lle's own shape, magnified: 640x480 into 1280x960 (2x) and into
    //    1000x750 (1.5625x).
    const Frame n64 = test_frame(640, 480);
    const auto d = run("n64_2x", n64, 4, 3, 1280, 960);
    check(differing(get(d, "bilinear"), get(d, "before")) == 0, "640x480 2x: bilinear byte-identical");
    check(differing(get(d, "nearest"), replicate(n64, 2)) == 0, "640x480 2x: nearest is replication");
    const auto e = run("n64_1.56x", n64, 4, 3, 1000, 750);
    check(differing(get(e, "bilinear"), get(e, "before")) == 0,
          "640x480 1.56x: bilinear byte-identical");

    // 4. A raised internal resolution: 2560x1920 into 1000x750 (0.39x).
    //    Minified through mipmaps now; the old GL_LINEAR alone is kept beside it.
    const Frame big = test_frame(2560, 1920);
    const auto g = run("big_0.39x", big, 4, 3, 1000, 750);
    std::printf("0.39x: bilinear (mipmapped) differs from before in %zu of %d pixels\n",
                differing(get(g, "bilinear"), get(g, "before")), 1000 * 750);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("hub_picture: all checks passed; images in %s\n", out.string().c_str());
    return 0;
}
