# Replays

F7 opens the save menu. Select Replays, or press Tab to switch between save
states and replays. Both tabs remain labeled; the active tab, page heading and
left-rail marker identify the current page. The menu and its actions support
keyboard and controller navigation. F11 starts/stops recording; Function + West is its default
controller shortcut. Hotkeys remain rebindable in settings. Fullscreen is
Alt+Enter.

Recording starts at a saved and restored mid-game anchor. The red dot and REC
label appear only after the runner confirms that recording started. F11 again
seals the movie; the menu reports saved or refused. Quit also seals completed
fields. Interrupted recordings keep a `.partial` file for explicit recovery.

Replays live in the title's save directory under `replays`. They use the same
rounded thumbnail cards as save states. Select a card for its title, date,
duration and Play/Export/Delete actions. Empty cards start a new recording;
the scrollable grid retains all recordings rather than overwriting twelve
slots. Cards are numbered in newest-first display order.

New UI recordings capture the current displayed frame before the anchor is
requested. The existing save-thumbnail path supplies the real preview pixels;
the displayed aspect is preserved in the card. Once recording seals, an
optional private `<movie>.preview` sidecar stores the thumbnail, title, date
and frame rate. Its checksum and movie-seal binding are checked before use.
Missing, damaged or unsupported sidecars show a placeholder; older movies
remain visible and playable. Their file date and recorded field count remain
available even without a preview. No thumbnail is invented or regenerated.
Preview creation failure does not discard a saved movie. Movie bytes and
checkpoint verification are unchanged.

Play starts a fresh runner process with the selected movie. Release controls once, then press a
button or move a stick to take over without resetting the game. That ends
verification with `taken_over`, rather than reporting a passing test. Quit
stops playback. Normal movie completion reports completion. Playback save
memory stays private, even after takeover.

Export copy writes the movie and any preview sidecar to new files under
`replays/exports`. Delete requires confirmation and removes the selected movie
and its sidecar. Neither action overwrites
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
