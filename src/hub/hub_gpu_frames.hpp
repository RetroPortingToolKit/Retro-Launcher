#pragma once

#include "core_link.hpp"
#include <memory>

namespace retcomm::hub {
class Picture;

// Presentation only. Capabilities are sampled with the actual GL context
// current, once before runner initialization; ownership events never change it.
class GpuFrames {
public:
    struct Stats { std::uint64_t imports = 0, pictures = 0, releases = 0, refused = 0; };
    GpuFrames();
    ~GpuFrames();
    GpuFrames(const GpuFrames&) = delete;
    GpuFrames& operator=(const GpuFrames&) = delete;
    rcore_gpu_capabilities capabilities() const;
    Stats stats() const;
    std::uint64_t pump(retro::corelink::CoreLink&, Picture&, std::uint64_t newest);
    // The GL context must remain current until release returns.
    void release(retro::corelink::CoreLink&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
