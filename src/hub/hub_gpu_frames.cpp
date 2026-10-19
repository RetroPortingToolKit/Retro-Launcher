#include "hub/hub_gpu_frames.hpp"
#include "hub/hub_picture.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <type_traits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "hub/hub_gl.hpp"
#include <SDL3/SDL_opengl_glext.h>
#endif

namespace retcomm::hub {
using retro::corelink::CoreLink;
using retro::corelink::GpuEvent;

struct GpuFrames::Impl {
    rcore_gpu_capabilities caps{};
    bool sampled = false;
    Stats stats;
#if defined(_WIN32)
    PFNGLGETSTRINGIPROC GetStringi = nullptr;
    PFNGLGETUNSIGNEDBYTEVEXTPROC GetUnsignedBytevEXT = nullptr;
    PFNGLGETUNSIGNEDBYTEI_VEXTPROC GetUnsignedBytei_vEXT = nullptr;
    PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT = nullptr;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT = nullptr;
    PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC ImportMemoryWin32HandleEXT = nullptr;
    PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT = nullptr;
    PFNGLGENSEMAPHORESEXTPROC GenSemaphoresEXT = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphoresEXT = nullptr;
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC ImportSemaphoreWin32HandleEXT = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC WaitSemaphoreEXT = nullptr;
    PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphoreEXT = nullptr;
    PFNGLFENCESYNCPROC FenceSync = nullptr;
    PFNGLCLIENTWAITSYNCPROC ClientWaitSync = nullptr;
    PFNGLDELETESYNCPROC DeleteSync = nullptr;
    struct Image {
        GLuint memory = 0, texture = 0, sem[2]{};
        GLsync fence = nullptr;
        std::uint64_t frame = 0;
    };
    struct Ring {
        rcore_gpu_ring desc{};
        Image images[RCORE_GPU_MAX_IMAGES]{};
        bool retiring = false;
    };
    std::map<std::uint64_t, Ring> rings;

    bool load() {
        bool ok = true;
        auto get = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(SDL_GL_GetProcAddress(name));
            ok = ok && f != nullptr;
        };
#define GPU_GL(name) get(name, "gl" #name)
        GPU_GL(GetStringi); GPU_GL(GetUnsignedBytevEXT); GPU_GL(GetUnsignedBytei_vEXT);
        GPU_GL(CreateMemoryObjectsEXT); GPU_GL(DeleteMemoryObjectsEXT);
        GPU_GL(MemoryObjectParameterivEXT); GPU_GL(ImportMemoryWin32HandleEXT);
        GPU_GL(TexStorageMem2DEXT); GPU_GL(GenSemaphoresEXT); GPU_GL(DeleteSemaphoresEXT);
        GPU_GL(ImportSemaphoreWin32HandleEXT); GPU_GL(WaitSemaphoreEXT);
        GPU_GL(SignalSemaphoreEXT); GPU_GL(FenceSync); GPU_GL(ClientWaitSync); GPU_GL(DeleteSync);
#undef GPU_GL
        if (!ok) return false;
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (const char* wanted : {"GL_EXT_memory_object", "GL_EXT_memory_object_win32",
                                   "GL_EXT_semaphore", "GL_EXT_semaphore_win32"}) {
            bool found = false;
            for (GLint i = 0; i < count; ++i)
                if (!std::strcmp(reinterpret_cast<const char*>(GetStringi(GL_EXTENSIONS, i)), wanted)) found = true;
            if (!found) return false;
        }
        return true;
    }

    void destroy(Ring& ring) {
        for (auto& im : ring.images) {
            if (im.fence) DeleteSync(im.fence);
            if (im.texture) glDeleteTextures(1, &im.texture);
            if (im.sem[0] || im.sem[1]) DeleteSemaphoresEXT(2, im.sem);
            if (im.memory) DeleteMemoryObjectsEXT(1, &im.memory);
            im = {};
        }
    }

    bool import(Ring& ring, const GpuEvent& e) {
        const auto& d = e.ring;
        if (!caps.handle_types || rings.size() >= 2 || !d.generation || rings.count(d.generation) ||
            d.struct_size != sizeof(d) || d.handle_type != RCORE_GPU_HANDLE_WIN32_NT ||
            d.sync_type != RCORE_GPU_SYNC_BINARY || d.pixel_format != RCORE_GPU_FORMAT_RGBA8 ||
            d.flags != RCORE_GPU_ORIGIN_TOP_LEFT || !d.width || !d.height ||
            d.width > caps.max_width || d.height > caps.max_height || !d.image_count ||
            d.image_count > RCORE_GPU_MAX_IMAGES || e.handles.size() != d.image_count * 3 ||
            std::memcmp(d.device_uuid, caps.device_uuid, 16) ||
            std::memcmp(d.device_luid, caps.device_luid, 8) ||
            d.device_node_mask != caps.device_node_mask) return false;
        ring.desc = d;
        // NT imports retain their own reference; GpuEvent closes the received
        // handles after this call. No inherited raw producer handle is used.
        for (unsigned i = 0; i < d.image_count; ++i) {
            if (d.images[i].allocation_size < std::uint64_t(d.width) * d.height * 4) return false;
            auto& im = ring.images[i];
            CreateMemoryObjectsEXT(1, &im.memory);
            const GLint dedicated = GL_TRUE;
            MemoryObjectParameterivEXT(im.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
            ImportMemoryWin32HandleEXT(im.memory, d.images[i].allocation_size,
                                       GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                       reinterpret_cast<void*>(e.handles[i * 3]));
            glGenTextures(1, &im.texture);
            glBindTexture(GL_TEXTURE_2D, im.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
            TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, d.width, d.height, im.memory, 0);
            GenSemaphoresEXT(2, im.sem);
            ImportSemaphoreWin32HandleEXT(im.sem[0], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                           reinterpret_cast<void*>(e.handles[i * 3 + 1]));
            ImportSemaphoreWin32HandleEXT(im.sem[1], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                           reinterpret_cast<void*>(e.handles[i * 3 + 2]));
            if (glGetError() != GL_NO_ERROR) return false;
        }
        return true;
    }

    void collect(CoreLink& link) {
        for (auto it = rings.begin(); it != rings.end();) {
            auto& ring = it->second;
            std::uint32_t released = 0;
            std::array<std::uint64_t, RCORE_GPU_MAX_IMAGES> frames{};
            bool busy = false;
            for (unsigned i = 0; i < ring.desc.image_count; ++i) {
                auto& im = ring.images[i];
                if (!im.fence) continue;
                const GLenum status = ClientWaitSync(im.fence, 0, 0);
                if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                    DeleteSync(im.fence);
                    im.fence = nullptr;
                    released |= 1u << i;
                    frames[i] = im.frame;
                    ++stats.releases;
                } else {
                    busy = true;
                    if (status == GL_WAIT_FAILED)
                        link.gpu_status(it->first, RCORE_GPU_RING_REFUSED, 0);
                }
            }
            if (released) link.gpu_status(it->first, RCORE_GPU_RING_READY, released, frames);
            if (ring.retiring && !busy) {
                destroy(ring);
                link.gpu_status(it->first, RCORE_GPU_RING_RETIRED, 0);
                it = rings.erase(it);
            } else ++it;
        }
    }
#endif
};

GpuFrames::GpuFrames() : impl_(std::make_unique<Impl>()) {}
GpuFrames::~GpuFrames() = default;
GpuFrames::Stats GpuFrames::stats() const { return impl_->stats; }

rcore_gpu_capabilities GpuFrames::capabilities() const {
    auto& s = *impl_;
    if (s.sampled) return s.caps;
    s.sampled = true;
    s.stats = {};
    s.caps.struct_size = sizeof(s.caps);
#if defined(_WIN32)
    if (!s.load()) return s.caps;
    s.GetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, s.caps.device_luid);
    s.GetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, s.caps.device_uuid);
    GLint node = 0, size = 0;
    glGetIntegerv(GL_DEVICE_NODE_MASK_EXT, &node);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
    if (glGetError() != GL_NO_ERROR || node <= 0 || size <= 0) return s.caps;
    s.caps.device_node_mask = node;
    s.caps.max_width = std::min<std::uint32_t>(size, retro::corelink::kMaxFrameWidth);
    s.caps.max_height = std::min<std::uint32_t>(size, retro::corelink::kMaxFrameHeight);
    s.caps.max_images = RCORE_GPU_MAX_IMAGES;
    s.caps.handle_types = RCORE_GPU_HANDLE_WIN32_NT;
    s.caps.sync_types = RCORE_GPU_SYNC_BINARY;
#endif
    return s.caps;
}

std::uint64_t GpuFrames::pump(CoreLink& link, Picture& picture, std::uint64_t newest) {
#if defined(_WIN32)
    auto& s = *impl_;
    s.collect(link);
    while (auto event = link.poll_gpu_event()) {
        const auto& e = *event;
        if (e.kind == GpuEvent::Kind::Register) {
            Impl::Ring ring;
            if (s.import(ring, e)) {
                ++s.stats.imports;
                s.rings.emplace(e.ring.generation, ring);
                link.gpu_status(e.ring.generation, RCORE_GPU_RING_READY, 0);
            } else {
                ++s.stats.refused;
                s.destroy(ring);
                SDL_Log("retro-hub: GPU ring import refused; using CPU frames");
                link.gpu_status(e.ring.generation, RCORE_GPU_RING_REFUSED, 0);
            }
        } else if (e.kind == GpuEvent::Kind::Unregister) {
            auto found = s.rings.find(e.generation);
            if (found != s.rings.end()) found->second.retiring = true;
            else link.gpu_status(e.generation, RCORE_GPU_RING_RETIRED, 0);
        } else {
            const auto& f = e.frame;
            auto found = s.rings.find(f.generation);
            if (found == s.rings.end() || f.image_index >= found->second.desc.image_count) {
                link.gpu_status(f.generation, RCORE_GPU_RING_REFUSED, 0);
                continue;
            }
            auto& ring = found->second;
            auto& im = ring.images[f.image_index];
            if (im.fence || ring.retiring) {
                link.gpu_status(f.generation, RCORE_GPU_RING_REFUSED, 0);
                continue;
            }
            const GLenum layout = GL_LAYOUT_GENERAL_EXT;
            s.WaitSemaphoreEXT(im.sem[0], 0, nullptr, 1, &im.texture, &layout);
            // Even an obsolete picture consumes ready and returns release.
            if (f.frame_number > newest) {
                if (picture.copy_texture(im.texture, ring.desc.width,
                        ring.desc.height, f.aspect_num, f.aspect_den)) {
                    newest = f.frame_number;
                    ++s.stats.pictures;
                } else {
                    ++s.stats.refused;
                    link.gpu_status(f.generation, RCORE_GPU_RING_REFUSED, 0);
                }
            }
            s.SignalSemaphoreEXT(im.sem[1], 0, nullptr, 1, &im.texture, &layout);
            im.fence = s.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            im.frame = f.frame_number;
            glFlush();
            if (!im.fence || glGetError() != GL_NO_ERROR)
                link.gpu_status(f.generation, RCORE_GPU_RING_REFUSED, 0);
        }
    }
    s.collect(link);
#else
    (void)picture;
    while (auto e = link.poll_gpu_event())
        link.gpu_status(e->kind == GpuEvent::Kind::Register ? e->ring.generation : e->generation,
                         RCORE_GPU_RING_REFUSED, 0);
#endif
    return newest;
}

void GpuFrames::release(CoreLink& link) {
#if defined(_WIN32)
    auto& s = *impl_;
    if (!s.rings.empty()) {
        // Shutdown only. Steady state uses nonblocking fence queries.
        glFinish();
        s.collect(link);
        for (auto& [id, ring] : s.rings) s.destroy(ring);
        s.rings.clear();
    }
#else
    (void)link;
#endif
    impl_->sampled = false;
    impl_->caps = {};
}
}
