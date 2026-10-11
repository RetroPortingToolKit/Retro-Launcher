# Overlay UI

One full-screen overlay for the hub's menus: a flat, semi-transparent dark layer
over what is behind it, a top bar, a rail of choices on the left, cards on the
right, and a legend of button hints along the bottom. It is the "Retro Overlay
Redesign" draft (the canvas the design was reviewed on), first slice.

**Switch.** On by default on Android. `RETCOMM_OVERLAY_UI=1` (or `0`) overrides it
on any platform, so the desktop can try it. Off, every menu is the old one.

## What is in this slice

| Surface | Overlay version | Code |
|---|---|---|
| Library menu (the drawer) | Rail: Home, Library settings, RomM sync, Add / scan files, Netplay, Check for updates, Close. Cards: launcher, catalog, status | `draw_overlay_menu`, `hub_main.cpp` |
| In-game quick menu | Rail: Resume, Save states, Show FPS, Close game. Cards: state (or the "not paused" banner in netplay), core, volume | `PlaySession::draw_menu_overlay`, `hub_play.cpp` |
| Shared chrome | scrim, top bar, rail, panel, cards, legend, the L3 + R3 hold | `hub_overlay.{hpp,cpp}` |

The library overlay shares the drawer's state (`drawer_open`, `drawer_t`), so
everything that keys off the drawer (the inert page behind it, Start, focus
hand-off) is unchanged.

## Ways in

- **Library:** L3 + R3 held 0.4 s; Start; the hamburger; a swipe in from the left
  edge. Android gesture navigation reads that swipe as Back, so
  `LauncherActivity` excludes a 40 dp strip on the left edge, 200 dp tall at the
  middle (`setSystemGestureExclusionRects`).
- **In a game:** L3 + R3, F1 or the Menu hotkey as before; on Android also a
  small corner button (`draw_menu_handle`).
- **Out:** B, Escape, Back, or a tap on the bare layer.

## Not in this slice

The Home screen, the player tabs on the Controls page, the Save states page, the
Netplay page and the updates page from the draft still use their existing
screens. Rail icons, the left-edge handle drawn while the menu is closed, and
LB / RB tab switching are not built.

## Checked

On the Pixel_10 emulator (x86_64): the library menu from the hamburger and the
edge swipe, a rail tap activating its page, Back closing it. Not checked: the
in-game quick menu and its corner button (the emulator has no game bundle),
gamepad input, and L3 + R3.
