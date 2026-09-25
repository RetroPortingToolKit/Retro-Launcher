/*
 * rcore.h — the host <-> engine-core contract.
 *
 * STATUS: DRAFT, 2026-09-23. Nothing implements this yet. Design and rationale:
 * docs/HOST_LIFECYCLE.md and docs/CORE_ABI.md. Until a first core and the
 * runner both build against it, every line here is a proposal.
 *
 * WHO IS WHO. A *core* is a shared library built by an engine (n64lle,
 * snesrecomp, psxrecomp, or a collaborator's). The *host* is the Retro
 * frontend. The core is never loaded into the host process: the generic
 * `retcomm-core-runner` loads it in a child process and implements the host
 * side of this header by forwarding over shared memory. A core cannot tell the
 * difference and must not try to.
 *
 * WHAT A CORE MUST NOT DO. Open a window, a GPU context, an audio device or an
 * input device; read POLICY settings from environment variables (instrument
 * knobs — dumps, censuses, trace logs — may stay environment-driven because
 * they change nothing the machine computes); resolve paths from
 * its own location or the working directory; write files outside the
 * directories the host hands it. Everything it needs arrives through this
 * header.
 *
 * VERSIONING.
 *   RCORE_ABI_MAJOR  changes on any incompatible edit. The host refuses a core
 *                    whose major differs, naming both numbers.
 *   RCORE_ABI_MINOR  changes on append-only edits: a new struct field at the
 *                    END, a new function pointer at the END of a table, a new
 *                    enum value, a new capability bit.
 * Every struct crossing the boundary begins with `uint32_t struct_size`, set by
 * whoever fills it, so either side can tell which appended fields exist.
 * Nothing crosses by value except scalars. No bool, no enum-typed fields
 * (their size is compiler-chosen): enums are named constants stored in
 * uint32_t.
 *
 * THREADING. The runner calls every rcore_core_api function from ONE thread,
 * the core thread. The core calls rcore_host_api functions only from that same
 * thread, except `log`, which is safe from any thread.
 */
#ifndef RCORE_H
#define RCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RCORE_ABI_MAJOR 0u /* 0 = draft; the first implemented contract is 1 */
#define RCORE_ABI_MINOR 0u
#define RCORE_DRAFT_REVISION 4u /* draft-only counter; see docs/CORE_ABI.md */

#if defined(_WIN32)
#  define RCORE_EXPORT __declspec(dllexport)
#else
#  define RCORE_EXPORT __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------------ */
/* Results                                                                   */
/* ------------------------------------------------------------------------ */

typedef int32_t rcore_result;
#define RCORE_OK                   0
#define RCORE_ERR_ABI             -1 /* ABI major mismatch */
#define RCORE_ERR_UNSUPPORTED     -2 /* capability not declared */
#define RCORE_ERR_CONTENT         -3 /* ROM / package rejected (message via log) */
#define RCORE_ERR_STATE           -4 /* called in the wrong lifecycle state */
#define RCORE_ERR_IO              -5
#define RCORE_ERR_INTERNAL        -6

/* ------------------------------------------------------------------------ */
/* Core description — also mirrored in the core's sidecar manifest, which is */
/* what the host reads at boot. The runner checks the two agree at load.     */
/* ------------------------------------------------------------------------ */

/* Capability bits. A capability not declared here is refused by the host,
 * never emulated. */
#define RCORE_CAP_RUN_FRAME        (1ull << 0) /* call-per-frame: run_frame() */
#define RCORE_CAP_OWNS_LOOP        (1ull << 1) /* run() owns the thread, yields at frame_boundary() */
#define RCORE_CAP_SAVESTATE        (1ull << 2) /* serialize/unserialize at a frame boundary */
#define RCORE_CAP_DETERMINISTIC    (1ull << 3) /* same inputs + same state => same state, bit-exact */
#define RCORE_CAP_ROLLBACK         (1ull << 4) /* DETERMINISTIC + the rb_* snapshot ring, run_frame_resim
                                                  and state_hash/state_hash_parts (rev 4). The
                                                  RUNNER owns the rollback session; the core
                                                  only provides these. */
#define RCORE_CAP_RESET            (1ull << 5)
#define RCORE_CAP_STRICT_MODE      (1ull << 6) /* honours RCORE_INIT_STRICT (bridges are fatal) */
#define RCORE_CAP_GAME_PACKAGE     (1ull << 7) /* loads a separate generated-code package */
#define RCORE_CAP_ACCESSORY_HOTPLUG (1ull << 8) /* accessory_changed() while running */
#define RCORE_CAP_GL_COMPUTE       (1ull << 9) /* wants a lent GL 4.3+ context (gl_get_proc_address);
                                                  must still run, in software, when none is lent */

typedef struct rcore_core_info {
    uint32_t struct_size;
    uint32_t abi_major;           /* RCORE_ABI_MAJOR the core was built with */
    uint32_t abi_minor;
    uint32_t _pad0;
    const char* core_id;          /* stable, e.g. "n64lle" */
    const char* core_version;     /* display only; identity is the host-computed file hash */
    const char* platforms;        /* comma-separated platform slugs, e.g. "n64" */
    uint64_t capabilities;        /* RCORE_CAP_* */

    /* Savestate compatibility. A state is loadable only when this string
     * matches EXACTLY the one recorded when the state was saved. NULL means
     * states are bound to this exact core file: the host compares its own
     * hash of the library instead. A core that claims a compat id promises
     * states survive every rebuild carrying the same id; if that is not
     * proven by a gate, leave it NULL. See docs/CORE_ABI.md, "Savestates". */
    const char* state_compat_id;
} rcore_core_info;

/* ------------------------------------------------------------------------ */
/* Video                                                                     */
/* ------------------------------------------------------------------------ */

#define RCORE_PIXEL_RGBA8          1u /* bytes R,G,B,A in memory order */

typedef struct rcore_frame {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;              /* bytes per row, >= width * 4 */
    uint32_t pixel_format;        /* RCORE_PIXEL_* */
    uint32_t aspect_num;          /* display aspect, e.g. 4:3; 0 = square pixels */
    uint32_t aspect_den;
    uint32_t _pad0;
    const void* pixels;           /* valid only for the duration of video_submit() */
    uint64_t frame_number;        /* core's own count, monotonically increasing */
} rcore_frame;

/* ------------------------------------------------------------------------ */
/* Input — the host sends a generic pad; the core maps it to its device.     */
/* ------------------------------------------------------------------------ */

#define RCORE_MAX_SEATS            8u

/* Generic button bits, laid out by position (south/east/west/north face
 * buttons), not by any console's labels. The core names each one it uses via
 * rcore_input_descriptor so the remap UI shows the console's own labels. */
#define RCORE_PAD_SOUTH            (1u << 0)
#define RCORE_PAD_EAST             (1u << 1)
#define RCORE_PAD_WEST             (1u << 2)
#define RCORE_PAD_NORTH            (1u << 3)
#define RCORE_PAD_L1               (1u << 4)
#define RCORE_PAD_R1               (1u << 5)
#define RCORE_PAD_L2               (1u << 6) /* digital view of the trigger */
#define RCORE_PAD_R2               (1u << 7)
#define RCORE_PAD_L3               (1u << 8)
#define RCORE_PAD_R3               (1u << 9)
#define RCORE_PAD_START            (1u << 10)
#define RCORE_PAD_SELECT           (1u << 11)
#define RCORE_PAD_DPAD_UP          (1u << 12)
#define RCORE_PAD_DPAD_DOWN        (1u << 13)
#define RCORE_PAD_DPAD_LEFT        (1u << 14)
#define RCORE_PAD_DPAD_RIGHT       (1u << 15)
/* The guide button is never delivered: it belongs to the host's overlay. */

#define RCORE_AXIS_LX              0u
#define RCORE_AXIS_LY              1u /* positive = up */
#define RCORE_AXIS_RX              2u
#define RCORE_AXIS_RY              3u
#define RCORE_AXIS_LT              4u /* 0..32767 */
#define RCORE_AXIS_RT              5u
#define RCORE_AXIS_COUNT           6u

typedef struct rcore_pad {
    uint32_t struct_size;
    uint32_t connected;           /* 0 = no device in this seat */
    uint32_t buttons;             /* RCORE_PAD_* */
    uint32_t _pad0;
    int16_t  axes[RCORE_AXIS_COUNT]; /* -32768..32767 */
} rcore_pad;

typedef struct rcore_input_descriptor {
    uint32_t struct_size;
    uint32_t button;              /* one RCORE_PAD_* bit, or 0 with axis set */
    uint32_t axis;                /* RCORE_AXIS_* + 1, or 0 */
    int32_t  axis_direction;      /* 0 = the whole axis; +1 / -1 = only that half,
                                     e.g. RX -1 "C-Left", RX +1 "C-Right" */
    const char* label;            /* the console's name for it, e.g. "C-Up" */
} rcore_input_descriptor;

/* ------------------------------------------------------------------------ */
/* Accessories — things plugged into a seat's controller or port: memory     */
/* paks, rumble paks, Transfer Paks, memory cards, multitap-side devices.    */
/* ------------------------------------------------------------------------ */

#define RCORE_MAX_ACCESSORY_SLOTS  2u /* per seat */

#define RCORE_ACC_FLAG_CONTENT     (1u << 0) /* needs a content file (e.g. a Game Boy ROM) */
#define RCORE_ACC_FLAG_SAVE        (1u << 1) /* owns save memory, declared at load/plug time */
#define RCORE_ACC_FLAG_NETPLAY     (1u << 2) /* affects simulation: joins the netplay match key */

/* Declared by the core: what may be plugged where. */
typedef struct rcore_accessory_type {
    uint32_t struct_size;
    uint32_t flags;               /* RCORE_ACC_FLAG_* */
    uint32_t seat_mask;           /* bit n set = seat n accepts it */
    uint32_t slot_mask;           /* bit n set = slot n accepts it */
    const char* id;               /* stable, e.g. "n64.transfer_pak" */
    const char* label;            /* e.g. "Transfer Pak" */
    const char* content_extensions; /* CONTENT only: comma-separated, e.g. ".gb,.gbc" */
} rcore_accessory_type;

/* Chosen by the host: what IS plugged where. Content is host-verified, like
 * the main content; the host hashes it and records the hash in savestates. */
typedef struct rcore_accessory_binding {
    uint32_t struct_size;
    uint32_t seat;
    uint32_t slot;
    uint32_t _pad0;
    const char* type_id;          /* an rcore_accessory_type.id */
    const char* content_path;     /* CONTENT only, else NULL */
    const char* content_sha256;   /* CONTENT only, else NULL */
} rcore_accessory_binding;

/* ------------------------------------------------------------------------ */
/* Options — replaces environment-variable settings entirely.               */
/* ------------------------------------------------------------------------ */

#define RCORE_OPT_ENUM             1u
#define RCORE_OPT_BOOL             2u
#define RCORE_OPT_INT              3u
#define RCORE_OPT_STRING           4u /* free text: names, hex addresses, paths inside title_dir */

#define RCORE_OPT_FLAG_RESTART     (1u << 0) /* takes effect only on next load */
#define RCORE_OPT_FLAG_NETPLAY     (1u << 1) /* affects simulation: part of the netplay match key */
#define RCORE_OPT_FLAG_DEVELOPER   (1u << 2) /* hidden unless the host's developer mode is on */

typedef struct rcore_option {
    uint32_t struct_size;
    uint32_t type;                /* RCORE_OPT_* */
    uint32_t flags;               /* RCORE_OPT_FLAG_* */
    uint32_t _pad0;
    const char* key;              /* stable, e.g. "video.widescreen" */
    const char* label;
    const char* description;
    const char* default_value;    /* always a string: "1", "fast", "60". NULL = unset:
                                     the core applies its own built-in default */
    const char* const* values;    /* ENUM: NULL-terminated list; else NULL */
    int64_t int_min;              /* INT only */
    int64_t int_max;
} rcore_option;

/* ------------------------------------------------------------------------ */
/* Save memory — host-owned, so a core crash cannot lose a save.             */
/* ------------------------------------------------------------------------ */

#define RCORE_SAVE_BATTERY         1u /* SRAM / FlashRAM */
#define RCORE_SAVE_EEPROM          2u
#define RCORE_SAVE_MEMCARD         3u /* controller pak / memory card */
#define RCORE_SAVE_ACCESSORY       4u /* an accessory's own save, e.g. the Game Boy
                                         cart battery behind a Transfer Pak */

typedef struct rcore_save_region {
    uint32_t struct_size;
    uint32_t kind;                /* RCORE_SAVE_* */
    uint32_t seat;                /* MEMCARD / ACCESSORY: the seat; else 0 */
    uint32_t slot;                /* ACCESSORY: the slot; else 0 */
    const char* id;               /* stable file stem, e.g. "eeprom", "cpak1".
                                     For ACCESSORY the host keys the file by the
                                     accessory content's hash too, so a Game Boy
                                     save follows its cartridge, not the seat.
                                     One accessory may own several regions, e.g.
                                     "tpak1" (battery RAM) and "tpak1.rtc"
                                     (MBC3 clock), so the battery file stays the
                                     standard format other emulators read. */
    uint64_t size;
    uint32_t erase_value;         /* low byte fills a region that has never been
                                     saved: 0xFF for EEPROM and flash, 0x00 if the
                                     hardware powers up zeroed. The core never
                                     learns whether a region is fresh. */
    uint32_t _pad1;
} rcore_save_region;

/* ------------------------------------------------------------------------ */
/* Diagnostics — the doctrine's loud misses and honest bridges.             */
/* ------------------------------------------------------------------------ */

#define RCORE_LOG_ERROR            1u
#define RCORE_LOG_WARN             2u
#define RCORE_LOG_INFO             3u
#define RCORE_LOG_DEBUG            4u

#define RCORE_EVENT_DISPATCH_MISS  1u /* uncovered code reached; host shows it, run continues only if bridged */
#define RCORE_EVENT_BRIDGE         2u /* interpreter bridged an uncovered range */
#define RCORE_EVENT_FAULT          3u /* core is about to stop; detail says why */

typedef struct rcore_event {
    uint32_t struct_size;
    uint32_t kind;                /* RCORE_EVENT_* */
    uint64_t guest_address;
    uint64_t frame_number;
    const char* detail;           /* copied by the host before returning */
} rcore_event;

/* ------------------------------------------------------------------------ */
/* Host API — implemented by the runner, called by the core.                */
/* ------------------------------------------------------------------------ */

typedef struct rcore_host_api {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t _pad0;
    void* host_ctx;               /* passed back as the first argument of every call */

    void (*log)(void* host_ctx, uint32_t level, const char* message);
    void (*report)(void* host_ctx, const rcore_event* event);

    /* Copies the frame out before returning. */
    void (*video_submit)(void* host_ctx, const rcore_frame* frame);
    /* Interleaved S16 stereo at the rate given by set_audio_rate(). */
    void (*audio_push)(void* host_ctx, const int16_t* samples, uint32_t frame_count);
    void (*set_audio_rate)(void* host_ctx, uint32_t sample_rate);

    /* State of `seat` as of the current frame; identical for every call within
     * one frame, which is what makes rollback replay exact. */
    void (*input_get)(void* host_ctx, uint32_t seat, rcore_pad* out);
    void (*rumble)(void* host_ctx, uint32_t seat, uint16_t low, uint16_t high);

    /* Current value of an option the core declared, as a string. Stable for
     * the whole frame. NULL means unset: the core applies its built-in
     * default. Asking for an undeclared key is a contract violation: the
     * runner ends the session with a fault naming the key, so NULL can only
     * ever mean "unset". */
    const char* (*option_get)(void* host_ctx, const char* key);
    /* Non-zero when any option changed since the last call. */
    uint32_t (*options_changed)(void* host_ctx);

    /* Host-owned backing store for a declared save region. The pointer is
     * valid until unload. The host persists it; the core never writes save
     * files itself. */
    void* (*save_memory)(void* host_ctx, const char* region_id);

    /* OWNS_LOOP cores only: call once per emulated frame, after video_submit.
     * May block (pause). Returns 0 to continue, non-zero when run() must
     * return. Inside this call the host may re-enter serialize/unserialize/
     * reset — it is the core's safe point. */
    uint32_t (*frame_boundary)(void* host_ctx);

    /* --- appended in draft revision 3 --- */

    /* NULL = no GL lent; a CAP_GL_COMPUTE core then runs its software path
     * and says so through log(). Non-NULL = a headless GL 4.3+ core-profile
     * context is current on the core thread for every core call. The core
     * never makes it current elsewhere, swaps it or presents from it: frames
     * still leave through video_submit. */
    void* (*gl_get_proc_address)(void* host_ctx, const char* name);

    /* --- appended in draft revision 4 --- */

    /* Wall-clock time as Unix microseconds -- the ONLY source a core may use
     * for anything guest-visible that follows real time (a cartridge RTC, a
     * BIOS time-of-day). Offline the host returns real time. In a netplay or
     * replay session it returns a pure function of the session's agreed epoch
     * and the frame number, identical on every peer, so a clock cartridge no
     * longer breaks DETERMINISTIC. A core never reads the system clock for
     * guest state. */
    uint64_t (*wall_clock_us)(void* host_ctx);
} rcore_host_api;

/* ------------------------------------------------------------------------ */
/* Load parameters                                                           */
/* ------------------------------------------------------------------------ */

#define RCORE_INIT_STRICT          (1u << 0) /* any bridge is fatal */
#define RCORE_INIT_NETPLAY         (1u << 1) /* requires CAP_DETERMINISTIC. Rollback sessions
                                                also require CAP_ROLLBACK; otherwise the
                                                session is delay-based lockstep. */

typedef struct rcore_init_params {
    uint32_t struct_size;
    uint32_t flags;               /* RCORE_INIT_* */
    const char* system_dir;       /* firmware / BIOS the host verified; read-only */
    const char* cache_dir;        /* core-private, writable, may be wiped */
} rcore_init_params;

typedef struct rcore_load_params {
    uint32_t struct_size;
    uint32_t _pad0;
    const char* content_path;     /* the player's ROM / disc image, host-verified */
    const char* content_sha256;   /* hex; what the host verified it against */
    const char* package_path;     /* generated-code package (CAP_GAME_PACKAGE), else NULL */
    const char* title_dir;        /* the title's data: game.toml, maps, enabled mods */
    const rcore_accessory_binding* accessories; /* plugged at power-on; may be NULL */
    uint32_t accessory_count;
    uint32_t _pad1;
} rcore_load_params;

/* ------------------------------------------------------------------------ */
/* Core API — implemented by the core, called by the runner.                */
/* ------------------------------------------------------------------------ */

typedef struct rcore_core_api {
    uint32_t struct_size;
    uint32_t _pad0;
    const rcore_core_info* info;

    /* Declarations the host reads before load. Arrays are core-owned and live
     * until deinit. */
    const rcore_option* (*options)(uint32_t* count);
    const rcore_input_descriptor* (*input_descriptors)(uint32_t* count);

    rcore_result (*init)(const rcore_host_api* host, const rcore_init_params* params);
    /* Declares save regions via the out-array once content is known. */
    rcore_result (*load)(const rcore_load_params* params,
                         const rcore_save_region** save_regions, uint32_t* save_region_count);

    /* Exactly one of these is non-NULL, matching the declared capability.
     * run_frame: emulate one frame and return.
     * run: loop until frame_boundary() says stop. */
    rcore_result (*run_frame)(void);
    rcore_result (*run)(void);

    rcore_result (*reset)(void);                       /* CAP_RESET */
    /* Savestate bytes are the core's own format, integrity key included. The
     * host wraps them in an envelope recording what they are valid against
     * (docs/CORE_ABI.md, "Savestates"); the core never sees the envelope. */
    uint64_t     (*serialize_size)(void);              /* CAP_SAVESTATE */
    rcore_result (*serialize)(void* out, uint64_t size);
    rcore_result (*unserialize)(const void* in, uint64_t size);

    void (*unload)(void);
    void (*deinit)(void);

    /* --- appended in draft revision 2 --- */

    /* Declared before load, like options. NULL when the core has none. */
    const rcore_accessory_type* (*accessory_types)(uint32_t* count);
    /* CAP_ACCESSORY_HOTPLUG: plug (binding != NULL) or unplug (binding == NULL)
     * at a frame boundary while running. When the new accessory has
     * RCORE_ACC_FLAG_SAVE the core declares its region through the out
     * parameter and then fetches it with save_memory(). On unplug the host
     * persists the old region before this returns. */
    rcore_result (*accessory_changed)(uint32_t seat, uint32_t slot,
                                      const rcore_accessory_binding* binding,
                                      const rcore_save_region** save_region);

    /* --- appended in draft revision 3 --- */

    /* Optional, for netplay desync detection: a hash of the simulation state
     * at the current frame boundary. It must cover only what DETERMINISTIC
     * promises. It must not include state racing asynchronous workers (e.g.
     * RDRAM sampled mid-field). NULL = the host hashes serialize() output
     * instead, at a lower rate. */
    uint64_t (*state_hash)(void);

    /* --- appended in draft revision 4: rollback (CAP_ROLLBACK) ---
     *
     * THE RUNNER OWNS THE SESSION (ruling 2026-09-25): it binds recomp-net's
     * rb_driver once, for every core, and owns transport, lobby, identity,
     * input rows and the snapshots of host-owned save memory. A core provides
     * only the engine-specific pieces below, which map onto RNetRbHost:
     *
     *   rb_snap_save/load/has/oldest/drop_after  -> snap_*
     *   run_frame / run_frame_resim              -> RNET_RB_REPLAY_INCREMENTAL
     *   state_hash / state_hash_parts            -> digest_master / digest_parts
     *
     * Pads need no core hook: the runner decodes published rows into the
     * generic rcore_pad that input_get returns, and the core maps that to its
     * console exactly as it does offline.
     *
     * All of these are called only at a frame boundary, on the core thread.
     * The ring is the CORE's, sized by the core. A snapshot keyed T is the
     * state BEFORE frame T runs. It lives in memory only and is never a
     * player savestate. */
    rcore_result (*rb_snap_save)(uint32_t tick);
    rcore_result (*rb_snap_load)(uint32_t tick);        /* RCORE_ERR_STATE if not held */
    uint32_t     (*rb_snap_has)(uint32_t tick);
    uint32_t     (*rb_snap_oldest)(uint32_t* oldest);   /* 0 = ring empty */
    void         (*rb_snap_drop_after)(uint32_t tick);  /* a replay re-keyed the timeline */
    uint32_t     (*rb_snap_depth)(void);                /* ring capacity, for reporting */

    /* Emulate one frame exactly as run_frame does -- the same state
     * afterwards, bit for bit -- but produce nothing the player already saw or
     * heard: no video_submit, audio_push, rumble or per-frame instrument
     * output. input_get still returns the published rows for that frame. */
    rcore_result (*run_frame_resim)(void);

    /* The digest broken into up to 3 named partitions, so a fork report names
     * the first partition that differs. Partition names are fixed per core
     * (e.g. "cpu", "rdram", "rsp"). state_hash must equal a pure function of
     * the parts. */
    void (*state_hash_parts)(uint64_t parts[3], const char* names[3]);
} rcore_core_api;

/* The single exported symbol. Returns NULL when the core cannot serve
 * `host_abi_major`, after logging nothing (the runner reports the mismatch). */
typedef const rcore_core_api* (*rcore_entry_fn)(uint32_t host_abi_major);
#define RCORE_ENTRY_SYMBOL "rcore_entry"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RCORE_H */
