# Shared GPU presentation

The Launcher can consume optional rcore draft 11 / CoreLink 2.4 shared images
from a separate runner process. The first path is a Windows Vulkan core with
an OpenGL frontend on the same actual adapter. It imports a ring once and
copies each accepted image on the GPU into the existing `Picture` texture.
Filtering, sharp bilinear, mipmaps, screenshots and the overlay continue
through `Picture`. Ordinary supported frames avoid CPU readback, pixel IPC
and texture upload. Explicit screenshots and save thumbnails may read back.

Actual UUID/LUID/node, extensions and limits are sampled once before core
initialization. Three slots and at most two generations bound resources.
The frontend waits ready, copies, signals released and flushes a completion
fence. It acknowledges a slot only once that fence completes. Link grant
numbers order pictures across rollback and CPU/GPU transitions; an obsolete
GPU frame still returns ownership. Failed imports or copies refuse that ring
and preserve CPU frames. Session teardown drains pending local GPU work and
releases imports before destroying the context. A new session samples its
own offer; there is no mid-match capability renegotiation.

The core must complete its ready signal before publishing a frame. This
prevents producer death after acceptance from stranding the frontend's GPU
wait. The frontend owns the final display copy, so it survives runner exit.
The precise ABI and handle lifetime contract is
[`Retro-Runtime/docs/GPU_FRAMES.md`](../third_party/Retro-Runtime/docs/GPU_FRAMES.md).
The Runtime pin in this change contains that coordinated implementation.

On other platforms, older Runtime/core versions, unsupported devices or
backends, and temporarily unavailable slots, the existing CPU frame path
continues to work. The supported OpenGL core renderer also keeps its existing
implementation. GPU transport does not alter guest memory or netplay policy.
In particular, n64lle's full-pixel-digest titles retain their portable online
renderer selection; this transport is not permission to omit guest pixels
from hashes or accept an unqualified graphics profile.

Private qualification uses the real `GpuFrames`, `Picture`, ImGui renderer
and separate CoreLink runner, rather than substituting a process-local texture
name. It checks every displayed picture, full canonical state and audio on a
selected Smash route, plus refusal, stale acknowledgments, slot exhaustion,
recovery and producer death. It also retains the existing enhanced first-frame
restore limitation: stale future pixels are invalidated, then native pixels
are displayed until enhanced history is rebuilt. Detailed source hashes,
completed-work timings, failures and platform limits are in n64lle's
`docs/evidence/DIRECT-GPU-PRESENTATION.md`. Hardware qualification is currently
one Windows NVIDIA adapter; cross-platform builds are not runtime proof.
