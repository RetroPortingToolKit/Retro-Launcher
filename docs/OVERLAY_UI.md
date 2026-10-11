# Overlay UI

One full-screen overlay for the hub's menus: a flat, semi-transparent dark layer
over what is behind it, a top bar, a rail of choices on the left, cards on the
right, and a legend of button hints along the bottom. It is the "Retro Overlay
Redesign" draft (the canvas the design was reviewed on), first slice.

**Switch.** On by default on Android. `RETCOMM_OVERLAY_UI=1` (or `0`) overrides it
on any platform, so the desktop can try it. Off, every menu is the old one.

## What is in it

| Surface | Overlay version | Code |
|---|---|---|
| The hub's frame | A shell: a top bar of tabs (Home, Library, Netplay, Updates, Settings), the page in the body, button hints along the bottom, and Save / Cancel pinned there while a settings page holds edits. B or a Back button steps out of a page | `draw_overlay_shell`, `hub_main.cpp` |
| Home | Continue playing, recently played (kept in `recent.json`), update and netplay summaries | `draw_shell_home` |
| Updates | Launcher, catalog and game updates, with their actions | `draw_shell_updates` |
| Settings | A list: library, RomM, add / scan files, one entry per platform, activity log | `draw_shell_settings` |
| In-game quick menu | Rail: Resume, Save states, Show FPS, Close game. The Save states page is the hub's own (`hub_states.{hpp,cpp}`): Save and Load tabs over twelve slots with thumbnails, an overwrite and delete confirmation, and the runner doing the work. The runtime's pixel browser is no longer used. Closing the menu holds back whatever was pressed, so Resume or a load never presses a button in the game. Cards: state (or the "not paused" banner in netplay), core, volume | `PlaySession::draw_menu_overlay`, `hub_play.cpp` |
| Shared chrome | scrim, top bar, tabs, rail, panel, cards, rows, legend, the L3 + R3 hold | `hub_overlay.{hpp,cpp}` |

The Library (platform cards, then a platform's covers with filter chips) and the game page (cover, state, Play, rows; "All options" opens the full older page) are redone. Mods, Netplay and every settings page are the
pages the hub already had, drawn in the shell's body; the overlay does not
restyle their insides yet. The drawer is gone in the shell: Start, L3 + R3 and
the edge swipe go Home.

## Ways in

- **Library:** L3 + R3 held 0.4 s; Start; the hamburger; a swipe in from the left
  edge. Android gesture navigation reads that swipe as Back, so
  `LauncherActivity` excludes a 40 dp strip on the left edge, 200 dp tall at the
  middle (`setSystemGestureExclusionRects`).
- **In a game:** L3 + R3, F1 or the Menu hotkey as before; on Android also a
  small corner button (`draw_menu_handle`).
- **Out:** B, Escape, Back, or a tap on the bare layer.

## Not redone yet

The insides of the old pages (Mods, the Netplay lobby, the settings pages, and
what "All options" opens) and the first-run wizard and dialogs keep their old look.
Rail icons and the left-edge handle drawn while the menu is closed are not
built.

## Checked

On the Pixel_10 emulator (x86_64): every tab, a settings page with its pinned
Save / Cancel, Back, and the edge swipe; the platform cards, a platform's cover
grid and a game page (in a test build with the Android title filter lifted, since
the catalog has no Android entries yet), and "All options". In game, on a bundle made from Retro-Runtime's
fake test core: Play, the quick menu from the corner button, and the Save states
page (save, load, overwrite confirmation, delete). Not checked: gamepad input,
L3 + R3, a real game's thumbnails.
