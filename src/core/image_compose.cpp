#include "retcomm/image_compose.hpp"

// The one implementation unit for stb_image in this project: hub_boxart.cpp
// includes the header and links against these. Keep the STBI_* options here in
// step with what that file expects (PNG + JPEG, no thread-locals).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace retcomm {
namespace {

struct Rgb {
    int r = 18, g = 22, b = 28;   // the launcher's own background, as a floor
};

std::vector<unsigned char> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

// The mat a box scan already sits on. Opaque border pixels only: art delivered
// with a transparent surround would otherwise average to black at the edges.
Rgb sample_border(const unsigned char* px, int w, int h) {
    long long sr = 0, sg = 0, sb = 0, n = 0;
    auto take = [&](int x, int y) {
        const unsigned char* p = px + (static_cast<size_t>(y) * w + x) * 4;
        if (p[3] < 128) return;
        sr += p[0];
        sg += p[1];
        sb += p[2];
        ++n;
    };
    for (int x = 0; x < w; ++x) {
        take(x, 0);
        take(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        take(0, y);
        take(w - 1, y);
    }
    Rgb out;
    if (n == 0) return out;
    out.r = static_cast<int>(sr / n);
    out.g = static_cast<int>(sg / n);
    out.b = static_cast<int>(sb / n);
    return out;
}

// Bilinear, in source pixel coordinates, with the source composited over the
// mat colour so a transparent scan does not darken at its edges.
void sample_bilinear(const unsigned char* px, int w, int h, float sx, float sy, const Rgb& bg,
                     unsigned char* out) {
    sx = std::clamp(sx, 0.f, static_cast<float>(w) - 1.f);
    sy = std::clamp(sy, 0.f, static_cast<float>(h) - 1.f);
    const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = sx - static_cast<float>(x0), fy = sy - static_cast<float>(y0);
    float acc[4] = {0, 0, 0, 0};
    const int xs[2] = {x0, x1};
    const int ys[2] = {y0, y1};
    const float wx[2] = {1.f - fx, fx};
    const float wy[2] = {1.f - fy, fy};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const unsigned char* p = px + (static_cast<size_t>(ys[j]) * w + xs[i]) * 4;
            const float k = wx[i] * wy[j];
            for (int c = 0; c < 4; ++c) acc[c] += k * static_cast<float>(p[c]);
        }
    const float a = acc[3] / 255.f;
    const float bgc[3] = {static_cast<float>(bg.r), static_cast<float>(bg.g),
                          static_cast<float>(bg.b)};
    for (int c = 0; c < 3; ++c)
        out[c] = static_cast<unsigned char>(
            std::clamp(acc[c] * a + bgc[c] * (1.f - a), 0.f, 255.f));
}

// One scaling pass of the source onto an RGB canvas. `scale` and the offsets
// decide fit vs cover; pixels outside the source take the mat colour.
void draw_scaled(const unsigned char* src, int sw, int sh, unsigned char* dst, int dw, int dh,
                 float scale, float off_x, float off_y, const Rgb& bg) {
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            unsigned char* o = dst + (static_cast<size_t>(y) * dw + x) * 3;
            const float fx = (static_cast<float>(x) + 0.5f - off_x) / scale - 0.5f;
            const float fy = (static_cast<float>(y) + 0.5f - off_y) / scale - 0.5f;
            if (fx < -0.5f || fy < -0.5f || fx > static_cast<float>(sw) - 0.5f ||
                fy > static_cast<float>(sh) - 0.5f) {
                o[0] = static_cast<unsigned char>(bg.r);
                o[1] = static_cast<unsigned char>(bg.g);
                o[2] = static_cast<unsigned char>(bg.b);
                continue;
            }
            sample_bilinear(src, sw, sh, fx, fy, bg, o);
        }
    }
}

// Separable box blur, running-sum so cost is independent of the radius. Three
// passes read as a Gaussian, which is all the background needs.
void box_blur(std::vector<unsigned char>& img, int w, int h, int radius, int passes) {
    if (radius < 1 || w < 2 || h < 2) return;
    std::vector<unsigned char> tmp(img.size());
    for (int pass = 0; pass < passes; ++pass) {
        for (int axis = 0; axis < 2; ++axis) {
            const int len = axis == 0 ? w : h;
            const int lines = axis == 0 ? h : w;
            const int step = axis == 0 ? 3 : w * 3;
            const int line_step = axis == 0 ? w * 3 : 3;
            const int r = std::min(radius, len - 1);
            const int window = 2 * r + 1;
            for (int l = 0; l < lines; ++l) {
                const unsigned char* in = img.data() + static_cast<size_t>(l) * line_step;
                unsigned char* out = tmp.data() + static_cast<size_t>(l) * line_step;
                int sum[3] = {0, 0, 0};
                for (int i = -r; i <= r; ++i) {
                    const int k = std::clamp(i, 0, len - 1);
                    for (int c = 0; c < 3; ++c) sum[c] += in[k * step + c];
                }
                for (int i = 0; i < len; ++i) {
                    for (int c = 0; c < 3; ++c)
                        out[i * step + c] = static_cast<unsigned char>(sum[c] / window);
                    const int add = std::clamp(i + r + 1, 0, len - 1);
                    const int sub = std::clamp(i - r, 0, len - 1);
                    for (int c = 0; c < 3; ++c)
                        sum[c] += in[add * step + c] - in[sub * step + c];
                }
            }
            img.swap(tmp);
        }
    }
}

void png_sink(void* ctx, void* data, int size) {
    auto* out = static_cast<std::string*>(ctx);
    out->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

}  // namespace

bool compose_image(const ImageComposeRequest& req, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (req.width <= 0 || req.height <= 0) return fail("bad target size");

    const std::vector<unsigned char> bytes = read_all(req.source);
    if (bytes.empty()) return fail("cannot read " + req.source.string());

    int sw = 0, sh = 0, comp = 0;
    stbi_uc* src = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &sw, &sh,
                                         &comp, 4);
    if (!src || sw <= 0 || sh <= 0) {
        if (src) stbi_image_free(src);
        return fail("cannot decode " + req.source.string() + " (png/jpeg only)");
    }

    const Rgb bg = sample_border(src, sw, sh);
    const float fit = std::min(static_cast<float>(req.width) / static_cast<float>(sw),
                               static_cast<float>(req.height) / static_cast<float>(sh));
    const float cover = std::max(static_cast<float>(req.width) / static_cast<float>(sw),
                                 static_cast<float>(req.height) / static_cast<float>(sh));
    auto centre = [&](float scale, float* ox, float* oy) {
        *ox = (static_cast<float>(req.width) - static_cast<float>(sw) * scale) * 0.5f;
        *oy = (static_cast<float>(req.height) - static_cast<float>(sh) * scale) * 0.5f;
    };

    std::vector<unsigned char> dst(static_cast<size_t>(req.width) * req.height * 3);
    float ox = 0.f, oy = 0.f;

    if (req.cover) {
        centre(cover, &ox, &oy);
        draw_scaled(src, sw, sh, dst.data(), req.width, req.height, cover, ox, oy, bg);
    } else if (req.blur_background && cover > fit) {
        // Background: the same art blown up to fill, blurred and dimmed so the
        // fitted copy in front of it reads as the subject rather than a second
        // picture.
        centre(cover, &ox, &oy);
        draw_scaled(src, sw, sh, dst.data(), req.width, req.height, cover, ox, oy, bg);
        box_blur(dst, req.width, req.height,
                 std::max(4, std::max(req.width, req.height) / 14), 3);
        for (unsigned char& v : dst) v = static_cast<unsigned char>(v * 45 / 100);

        std::vector<unsigned char> fg(dst.size());
        centre(fit, &ox, &oy);
        draw_scaled(src, sw, sh, fg.data(), req.width, req.height, fit, ox, oy, bg);
        const int x0 = static_cast<int>(std::floor(ox));
        const int y0 = static_cast<int>(std::floor(oy));
        const int x1 = static_cast<int>(std::ceil(ox + static_cast<float>(sw) * fit));
        const int y1 = static_cast<int>(std::ceil(oy + static_cast<float>(sh) * fit));
        for (int y = std::max(0, y0); y < std::min(req.height, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(req.width, x1); ++x) {
                const size_t i = (static_cast<size_t>(y) * req.width + x) * 3;
                dst[i] = fg[i];
                dst[i + 1] = fg[i + 1];
                dst[i + 2] = fg[i + 2];
            }
    } else {
        centre(fit, &ox, &oy);
        draw_scaled(src, sw, sh, dst.data(), req.width, req.height, fit, ox, oy, bg);
    }
    stbi_image_free(src);

    std::string png;
    if (!stbi_write_png_to_func(png_sink, &png, req.width, req.height, 3, dst.data(),
                                req.width * 3))
        return fail("png encode failed");

    std::error_code ec;
    if (!req.dest.parent_path().empty()) fs::create_directories(req.dest.parent_path(), ec);
    std::ofstream out(req.dest, std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write " + req.dest.string());
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    if (!out) return fail("short write to " + req.dest.string());
    return true;
}

}  // namespace retcomm
