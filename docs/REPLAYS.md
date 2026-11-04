# Replays

F7 opens the save menu. Select Replays, or press Tab to switch between save
states and replays. The menu and its actions support keyboard and controller
navigation. F11 starts/stops recording; Function + West is its default
controller shortcut. Hotkeys remain rebindable in settings. Fullscreen is
Alt+Enter.

Recording starts at a saved and restored mid-game anchor. The red dot and REC
label appear only after the runner confirms that recording started. F11 again
seals the movie; the menu reports saved or refused. Quit also seals completed
fields. Interrupted recordings keep a `.partial` file for explicit recovery.

Replays live in the title's save directory under `replays`. Play starts a fresh
runner process with the selected movie. Release controls once, then press a
button or move a stick to take over without resetting the game. That ends
verification with `taken_over`, rather than reporting a passing test. Quit
stops playback. Normal movie completion reports completion. Playback save
memory stays private, even after takeover.

Export copy writes a new file under `replays/exports`; Delete requires a second
confirmation and only removes the selected movie. Neither action overwrites
another replay. Movies contain private initial saves and a core snapshot; do
not publish them with source or bug reports.

This needs Retro-Runtime link 2.5 and a core with working file save states.
Older runners, netplay and accessory sessions refuse recording. Exact core,
runner, package, ROM and option identities are checked. Unsupported anchors,
changed options and mismatched checkpoints refuse or report divergence. A core
advertising save states is not proof of deterministic cross-process replay;
qualify its file-state support on the intended route. The runtime's
`docs/BOOT_ROUTES.md` documents headless playback and machine-readable verdicts.

The frontend builds and protocol tests cover control/status and replay startup.
Visual menu layout, audio-device output and physical-controller use need a
hands-on check; headless protocol tests do not establish those behaviors.
