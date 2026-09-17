#pragma once

#include "retcomm/paths.hpp"

#include <string>

namespace retcomm {

// Fit one image onto a canvas of a fixed size and write it out as a PNG.
//
// Store art is sized to slots real cover art never matches — a PlayStation
// jewel case is nearly square, a SNES box is landscape, and Steam wants a 2:3
// portrait, a 21:9 banner and a small icon. So the art is scaled to fit whole,
// never cropped and never stretched, and the margin is filled with the same art
// scaled to cover and blurred, the way store fronts do it. Averaging the
// source's border instead gives a muddy flat colour: on the Tomba cover it
// comes out olive, from a border that is part black spine and part sky.
struct ImageComposeRequest {
    fs::path source;      // PNG or JPEG
    fs::path dest;        // written as PNG, parent directories created
    int width = 0;
    int height = 0;
    // Fill the canvas and crop the overflow instead of fitting and padding.
    // Right for a banner cut from a large image, wrong for a box scan.
    bool cover = false;
    // Blurred fill behind a fitted image. Off gives a flat mat sampled from the
    // source border — quieter, and what an icon wants.
    bool blur_background = true;
};

bool compose_image(const ImageComposeRequest& req, std::string* error = nullptr);

}  // namespace retcomm
