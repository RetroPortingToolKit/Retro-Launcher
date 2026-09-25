/*
 * rcore_fake_core -- the smallest core that exercises the host: a silent core
 * that states a 50 Hz frame rate after load(), submits one 64x48 frame per
 * run_frame, and logs how many frames it ran at unload. A host pacing it
 * correctly runs it at 50 frames a second, not the 60 Hz fallback.
 *
 * Test fixture only; it exports rcore_entry and nothing else. Its sidecar
 * manifest is written beside it by CMake.
 */
#include "rcore/rcore.h"

#include <stdio.h>
#include <string.h>

#define W 64
#define H 48

static const rcore_host_api* g_host;
static unsigned long long g_frames;
static unsigned char g_pixels[W * H * 4];

static const rcore_core_info k_info = {
    sizeof(rcore_core_info), RCORE_ABI_MAJOR, RCORE_ABI_MINOR, 0,
    "fake", "1.0", "test", RCORE_CAP_RUN_FRAME, NULL,
};

static const rcore_option* opts(uint32_t* count) { *count = 0; return NULL; }
static const rcore_input_descriptor* descs(uint32_t* count) { *count = 0; return NULL; }

static rcore_result init(const rcore_host_api* host, const rcore_init_params* params) {
    (void)params;
    g_host = host;
    return RCORE_OK;
}

static rcore_result load(const rcore_load_params* params, const rcore_save_region** regions,
                         uint32_t* count) {
    (void)params;
    *regions = NULL;
    *count = 0;
    /* The rate is known once content is: state it (rev 5). */
    if (g_host->struct_size >= offsetof(rcore_host_api, set_frame_rate) + sizeof(void*) &&
        g_host->set_frame_rate) {
        g_host->set_frame_rate(g_host->host_ctx, 50, 1);
    }
    return RCORE_OK;
}

static rcore_result run_frame(void) {
    ++g_frames;
    memset(g_pixels, (int)(g_frames & 0xff), sizeof g_pixels);
    rcore_frame f;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.width = W;
    f.height = H;
    f.stride = W * 4;
    f.pixel_format = RCORE_PIXEL_RGBA8;
    f.pixels = g_pixels;
    f.frame_number = g_frames;
    g_host->video_submit(g_host->host_ctx, &f);
    return RCORE_OK;
}

static void unload(void) {
    char line[64];
    snprintf(line, sizeof line, "FAKE_DONE frames=%llu", g_frames);
    g_host->log(g_host->host_ctx, RCORE_LOG_INFO, line);
}

static void deinit(void) {}

static const rcore_core_api k_api = {
    sizeof(rcore_core_api), 0, &k_info, opts, descs, init, load, run_frame, NULL,
    NULL, NULL, NULL, NULL, unload, deinit,
};

RCORE_EXPORT const rcore_core_api* rcore_entry(uint32_t host_abi_major) {
    return host_abi_major == RCORE_ABI_MAJOR ? &k_api : NULL;
}
