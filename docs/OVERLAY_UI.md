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
| In-game quick menu | Rail: Resume, Save states, Show FPS, Close game. Cards: state (or the "not paused" banner in netplay), core, volume | `PlaySession::draw_menu_overlay`, `hub_play.cpp` |
| Shared chrome | scrim, top bar, tabs, rail, panel, cards, rows, legend, the L3 + R3 hold | `hub_overlay.{hpp,cpp}` |

The Library tab, a title's page, Mods, Netplay and every settings page are the
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

The insides of the old pages (title grid and title page, Mods, the Netplay lobby,
the settings pages) and the first-run wizard and dialogs keep their old look.
Rail icons, the left-edge handle drawn while the menu is closed, and the
in-game Save states page from the draft are not built.

## Checked

On the Pixel_10 emulator (x86_64): every tab, a settings page with its pinned
Save / Cancel, Back, and the edge swipe. Not checked: a title grid or title page
(the catalog has nothing for Android), the in-game menu (no game bundle),
gamepad input, L3 + R3.
