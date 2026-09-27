#!/usr/bin/env python3
"""Writes n64_controller.svg beside this file: a front-on N64 controller for
Retro Launcher's n64lle Gamepads tab and Configure page.

The OUTLINE is traced from a photograph of a real controller: segmented from
the table by colour, its left half mirrored for symmetry, smoothed and fitted
with curves (photo pixels scaled x2.2, the symmetry axis at x=400). The
centre prong is then narrowed to 80% below the arches (easing in over 70 px),
taking out the width the photo's perspective and shadow added to it. The
button positions are measured from the same photo under the same mapping.
No logo or trademark is drawn.

Render:  rsvg-convert -w 800 n64_controller.svg -o ../controllers/n64_controller.png
The feature centres below are the Configure page's chip anchors
(src/hub/hub_main.cpp, n64_pad_hits): keep the two in step.
"""
import os

W, H = 800, 760
OUTLINE = "M 373.6 60.6 C 388.3 59.8 411.7 59.8 426.4 60.6 C 441.1 61.4 449.1 62.9 461.6 65.4 C 474.1 67.9 493.4 73.3 501.2 75.6 C 509.0 77.9 506.5 77.7 508.6 79.1 C 510.7 80.4 511.3 79.2 513.6 83.7 C 516.0 88.2 520.3 101.2 522.7 106.0 C 525.1 110.7 525.8 110.8 528.1 112.2 C 530.4 113.7 525.9 113.1 536.4 114.7 C 546.9 116.4 578.4 120.0 591.4 122.1 C 604.4 124.3 606.1 124.5 614.5 127.5 C 622.9 130.5 633.6 135.1 642.0 140.2 C 650.4 145.4 657.7 150.9 664.9 158.2 C 672.2 165.6 680.4 176.3 685.6 184.3 C 690.8 192.3 693.4 199.3 696.1 206.3 C 698.8 213.3 700.0 216.6 701.8 226.1 C 703.6 235.6 704.3 249.2 706.9 263.5 C 709.6 277.8 714.8 295.4 717.5 311.9 C 720.2 328.4 722.6 346.7 723.1 362.5 C 723.7 378.3 721.4 397.3 720.7 406.5 C 720.1 415.7 720.7 409.2 719.1 417.5 C 717.6 425.8 715.3 441.7 711.5 456.0 C 707.7 470.3 700.4 492.3 696.4 503.3 C 692.4 514.3 690.1 517.3 687.4 522.0 C 684.7 526.7 683.4 528.5 680.2 531.3 C 677.0 534.1 672.8 536.8 668.4 538.5 C 664.0 540.2 659.1 541.2 654.1 541.6 C 649.1 542.0 643.1 541.8 638.7 540.7 C 634.3 539.7 631.4 538.2 627.8 535.2 C 624.2 532.3 620.7 528.2 617.2 523.1 C 613.6 518.0 611.0 514.1 606.5 504.4 C 602.0 494.7 594.9 477.8 590.4 464.8 C 585.8 451.8 581.8 436.6 579.4 426.3 C 577.0 416.0 576.6 409.4 576.1 403.2 C 575.6 397.0 575.8 393.6 576.4 389.2 C 577.0 384.7 579.4 379.8 579.6 376.7 C 579.9 373.5 578.5 371.5 577.6 370.0 C 576.8 368.5 576.0 368.1 574.5 367.5 C 572.9 366.8 573.4 365.8 568.3 366.1 C 563.2 366.4 549.9 368.1 543.9 369.2 C 538.0 370.4 536.1 370.9 532.5 372.9 C 528.8 374.9 526.9 375.8 522.0 381.2 C 517.2 386.7 508.3 397.3 503.3 405.4 C 498.4 413.5 495.8 411.8 492.3 429.6 C 488.8 447.4 485.5 489.7 482.4 512.1 C 479.3 534.5 475.7 552.2 473.7 563.8 C 471.6 575.3 472.7 571.0 470.1 581.4 C 467.5 591.9 461.9 614.4 458.0 626.5 C 454.1 638.6 450.4 646.3 446.7 654.0 C 443.0 661.7 439.5 667.7 435.9 672.7 C 432.2 677.6 428.3 681.3 424.6 683.6 C 421.0 686.0 420.5 686.3 414.1 686.8 C 407.6 687.4 392.4 687.4 385.9 686.8 C 379.5 686.3 379.0 686.0 375.4 683.6 C 371.7 681.3 367.8 677.6 364.1 672.7 C 360.5 667.7 357.0 661.7 353.3 654.0 C 349.6 646.3 345.9 638.6 342.0 626.5 C 338.1 614.4 334.0 600.5 329.9 581.4 C 325.8 562.3 321.3 537.4 317.6 512.1 C 313.9 486.8 311.2 447.4 307.7 429.6 C 304.2 411.8 300.8 412.5 296.7 405.4 C 292.6 398.2 286.2 390.7 283.1 386.7 C 279.9 382.7 280.6 383.5 278.0 381.2 C 275.4 378.9 271.2 374.9 267.5 372.9 C 263.9 370.9 262.0 370.4 256.1 369.2 C 250.1 368.1 237.1 366.2 231.7 366.1 C 226.3 366.0 225.3 367.9 223.8 368.6 C 222.2 369.2 222.9 368.6 222.4 370.0 C 221.8 371.3 220.1 373.5 220.4 376.7 C 220.6 379.8 223.0 384.7 223.6 389.2 C 224.2 393.6 224.4 397.0 223.9 403.2 C 223.4 409.4 223.0 416.0 220.6 426.3 C 218.2 436.6 214.2 451.8 209.6 464.8 C 205.1 477.8 198.0 494.7 193.5 504.4 C 189.0 514.1 186.4 518.0 182.8 523.1 C 179.3 528.2 175.8 532.3 172.2 535.2 C 168.6 538.2 165.7 539.7 161.3 540.7 C 156.9 541.8 150.8 542.0 145.9 541.6 C 140.9 541.2 136.0 540.2 131.6 538.5 C 127.2 536.8 123.0 534.1 119.8 531.3 C 116.6 528.5 115.3 526.7 112.6 522.0 C 109.9 517.3 105.8 508.2 103.6 503.3 C 101.4 498.4 102.1 500.2 99.6 492.3 C 97.0 484.4 91.9 470.3 88.5 456.0 C 85.1 441.7 81.2 420.8 79.3 406.5 C 77.3 392.2 77.2 377.5 76.8 370.2 C 76.4 362.9 75.9 372.2 76.9 362.5 C 77.8 352.8 79.8 328.4 82.5 311.9 C 85.2 295.4 90.4 277.8 93.1 263.5 C 95.7 249.2 96.4 235.6 98.2 226.1 C 100.0 216.6 101.2 213.3 103.9 206.3 C 106.6 199.3 109.2 192.3 114.4 184.3 C 119.6 176.3 127.8 165.6 135.1 158.2 C 142.3 150.9 149.6 145.4 158.0 140.2 C 166.4 135.1 177.1 130.5 185.5 127.5 C 193.9 124.5 195.6 124.3 208.6 122.1 C 221.6 120.0 253.1 116.4 263.6 114.7 C 274.1 113.1 269.6 113.7 271.9 112.2 C 274.2 110.8 274.9 110.7 277.3 106.0 C 279.7 101.2 284.0 88.2 286.4 83.7 C 288.7 79.2 289.3 80.4 291.4 79.1 C 293.5 77.7 291.0 77.9 298.8 75.6 C 306.6 73.3 325.9 67.9 338.4 65.4 C 350.9 62.9 358.9 61.4 373.6 60.6 Z"

DPAD = (193, 240)                     # centre; arms reach 53
START = (400, 262)
STICK = (400, 392)
B, A = (511, 260), (556, 309)
C_UP, C_LEFT, C_RIGHT, C_DOWN = (611, 189), (571, 225), (652, 225), (611, 262)


def button(c, r, fill, rim):
    x, y = c
    return f"""  <circle cx="{x}" cy="{y}" r="{r + rim}" fill="#a9aaae"/>
  <circle cx="{x}" cy="{y}" r="{r + rim}" fill="none" stroke="#8b8c91" stroke-width="1.2"/>
  <circle cx="{x}" cy="{y + 3}" r="{r}" fill="#000" opacity="0.35" filter="url(#blur1)"/>
  <circle cx="{x}" cy="{y}" r="{r}" fill="url(#{fill})"/>
  <ellipse cx="{x - r * 0.3:.1f}" cy="{y - r * 0.38:.1f}" rx="{r * 0.45:.1f}" ry="{r * 0.25:.1f}" fill="#fff" opacity="0.4"/>
"""


def svg():
    dx, dy = DPAD
    sx, sy = STICK
    out = f"""<?xml version="1.0" encoding="UTF-8"?>
<!-- Generated by n64_controller.py (see there). -->
<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <defs>
    <path id="body" d="{OUTLINE}"/>
    <clipPath id="bodyclip"><use href="#body"/></clipPath>
    <linearGradient id="plastic" gradientUnits="userSpaceOnUse" x1="0" y1="60" x2="0" y2="690">
      <stop offset="0" stop-color="#d5d6d9"/>
      <stop offset="0.35" stop-color="#c3c4c8"/>
      <stop offset="0.75" stop-color="#b2b3b8"/>
      <stop offset="1" stop-color="#9d9ea3"/>
    </linearGradient>
    <radialGradient id="glow" gradientUnits="userSpaceOnUse" cx="400" cy="230" r="340">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.22"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </radialGradient>
    <filter id="blur6" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="6"/></filter>
    <filter id="blur3" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="3"/></filter>
    <filter id="blur1" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="1.2"/></filter>
    <filter id="drop" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="12"/></filter>
    <radialGradient id="dish" cx="0.5" cy="0.45" r="0.55">
      <stop offset="0" stop-color="#b3b4b8"/><stop offset="0.85" stop-color="#bdbec2"/><stop offset="1" stop-color="#d9dadd"/>
    </radialGradient>
    <linearGradient id="cross" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#6d6f74"/><stop offset="1" stop-color="#46484c"/>
    </linearGradient>
    <radialGradient id="well" cx="0.5" cy="0.4" r="0.6">
      <stop offset="0" stop-color="#3a3c40"/><stop offset="0.75" stop-color="#55575c"/><stop offset="1" stop-color="#8f9095"/>
    </radialGradient>
    <radialGradient id="cap" cx="0.42" cy="0.38" r="0.7">
      <stop offset="0" stop-color="#e2e3e6"/><stop offset="0.7" stop-color="#b9babe"/><stop offset="1" stop-color="#8d8e93"/>
    </radialGradient>
    <radialGradient id="red" cx="0.4" cy="0.35" r="0.75"><stop offset="0" stop-color="#f06a62"/><stop offset="0.6" stop-color="#cf2127"/><stop offset="1" stop-color="#8e1016"/></radialGradient>
    <radialGradient id="green" cx="0.4" cy="0.35" r="0.75"><stop offset="0" stop-color="#5fd07a"/><stop offset="0.6" stop-color="#1a9a3c"/><stop offset="1" stop-color="#0d6327"/></radialGradient>
    <radialGradient id="blue" cx="0.4" cy="0.35" r="0.75"><stop offset="0" stop-color="#6d8fff"/><stop offset="0.6" stop-color="#2447c6"/><stop offset="1" stop-color="#142c85"/></radialGradient>
    <radialGradient id="yellow" cx="0.4" cy="0.35" r="0.75"><stop offset="0" stop-color="#ffe56a"/><stop offset="0.6" stop-color="#f3c20e"/><stop offset="1" stop-color="#b58a00"/></radialGradient>
  </defs>

  <use href="#body" fill="#000" opacity="0.5" filter="url(#drop)" transform="translate(6 16)"/>
  <use href="#body" fill="url(#plastic)"/>
  <g clip-path="url(#bodyclip)">
    <use href="#body" fill="none" stroke="#6f7075" stroke-opacity="0.55" stroke-width="30" filter="url(#blur6)"/>
    <use href="#body" fill="none" stroke="#ffffff" stroke-opacity="0.5" stroke-width="6" filter="url(#blur3)" transform="translate(0 5)"/>
    <rect x="0" y="0" width="{W}" height="{H}" fill="url(#glow)"/>
  </g>
  <use href="#body" fill="none" stroke="#7d7e83" stroke-width="1.5"/>

  <circle cx="{dx}" cy="{dy}" r="66" fill="url(#dish)"/>
  <circle cx="{dx}" cy="{dy}" r="66" fill="none" stroke="#9fa0a5" stroke-width="1.5" opacity="0.7"/>
  <g transform="translate({dx} {dy})">
    <path d="M-18 -53 H18 V-18 H53 V18 H18 V53 H-18 V18 H-53 V-18 H-18 Z" fill="#000" opacity="0.45" filter="url(#blur3)" transform="translate(2 5)"/>
    <path d="M-18 -50 Q-18 -54 -14 -54 H14 Q18 -54 18 -50 V-18 H50 Q54 -18 54 -14 V14 Q54 18 50 18 H18 V50 Q18 54 14 54 H-14 Q-18 54 -18 50 V18 H-50 Q-54 18 -54 14 V-14 Q-54 -18 -50 -18 H-18 Z" fill="url(#cross)" stroke="#34363a" stroke-width="1.2"/>
    <path d="M-14 -52 H14" stroke="#9a9ca1" stroke-width="1.5" opacity="0.8"/>
    <circle r="10" fill="#3e4044" opacity="0.9"/>
    <path d="M0 -44 l7 10 h-14 z M0 44 l7 -10 h-14 z M-44 0 l10 7 v-14 z M44 0 l-10 7 v-14 z" fill="#3a3c40" opacity="0.8"/>
  </g>
"""
    out += button(START, 20, "red", 6)
    out += f"""  <g transform="translate({sx} {sy})">
    <circle r="74" fill="#a2a3a8"/>
    <circle r="69" fill="url(#well)"/>
    <path d="M-26 -51 L26 -51 L51 -26 L51 26 L26 51 L-26 51 L-51 26 L-51 -26 Z" fill="#2c2e31" opacity="0.8"/>
    <circle r="34" fill="#000" opacity="0.5" filter="url(#blur3)" transform="translate(3 6)"/>
    <circle r="33" fill="url(#cap)" stroke="#7a7b80" stroke-width="1.2"/>
    <circle r="25" fill="none" stroke="#a3a4a9" stroke-width="1.6"/>
    <circle r="17" fill="none" stroke="#a9aaaf" stroke-width="1.4"/>
    <circle r="9" fill="none" stroke="#b0b1b6" stroke-width="1.2"/>
    <ellipse cx="-10" cy="-12" rx="12" ry="7" fill="#fff" opacity="0.3"/>
  </g>
"""
    # the engraved groove joining B and A, and the ring round the C buttons
    cx = (C_LEFT[0] + C_RIGHT[0]) / 2
    cy = (C_UP[1] + C_DOWN[1]) / 2
    out += f"""  <line x1="{B[0]}" y1="{B[1]}" x2="{A[0]}" y2="{A[1]}" stroke="#8e8f94" stroke-width="5" stroke-linecap="round"/>
  <line x1="{B[0]}" y1="{B[1] + 2}" x2="{A[0]}" y2="{A[1] + 2}" stroke="#e4e5e8" stroke-width="1.5" opacity="0.6"/>
  <circle cx="{cx}" cy="{cy}" r="58" fill="none" stroke="#98999e" stroke-width="2.5"/>
  <circle cx="{cx}" cy="{cy + 1.5}" r="58" fill="none" stroke="#e4e5e8" stroke-width="1" opacity="0.6"/>
"""
    out += button(B, 24, "green", 6) + button(A, 24, "blue", 6)
    for c in (C_UP, C_LEFT, C_RIGHT, C_DOWN):
        out += button(c, 18, "yellow", 4)
    (ux, uy), (lx, ly), (rx, ry), (bx, by) = C_UP, C_LEFT, C_RIGHT, C_DOWN
    out += f"""  <g fill="#b88b00" opacity="0.9">
    <path d="M{ux} {uy - 8} l6 9 h-12 z"/>
    <path d="M{bx} {by + 8} l6 -9 h-12 z"/>
    <path d="M{lx - 8} {ly} l9 -6 v12 z"/>
    <path d="M{rx + 8} {ry} l-9 -6 v12 z"/>
  </g>
</svg>
"""
    return out


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "n64_controller.svg"), "w") as f:
        f.write(svg())
