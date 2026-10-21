#!/usr/bin/env python3
"""Generate ui/main.json for the Magpie usage display.

One page == one subscription. ``MAGPIE_SLOTS_MAX`` pages are authored; the
application reveals as many as the companion reports and clamps swipes back to
the last live page. Paging itself is ESP-GSP's ``page_flow`` widget, so the
framework owns the drag, fling and settle behaviour instead of custom gesture
code.

Layout (480x480):

    header     MAGPIE / link status / tightest window          y 0..64
    page_flow  one subscription per page                       y 68..424
    footer     page dots + "n/m" counter                       y 424..480

Run from the project directory:

    python3 ui/generate.py            # writes ui/main.json
    python3 ui/generate.py --check    # fail if ui/main.json is stale
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "main.json"

SCREEN_W = 480
SCREEN_H = 480
SLOTS = 16
WINDOWS = 3

# Glyphs available to runtime-bound text. The scene needs digits (percentages,
# counters), ASCII letters (provider/plan/window names), the punctuation the
# formatters emit and "%".
CHARSET = (
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
)

# Palette: dark desk display, one accent per state.
BG = "#0E1216"
PANEL = "#151B21"
TRACK = "#243039"
INK = "#F2F5F7"
DIM = "#8A97A3"
ACCENT = "#FF4C01"
DOT = "#39454F"

HEADER_H = 64
FLOW_Y = 68
FLOW_H = 356
FOOTER_Y = FLOW_Y + FLOW_H

WINDOW_BLOCK_H = 96
WINDOW_TOP = 60


def label(name, x, y, w, h, text, size, color, align="left", bold=False,
          hidden=False, dynamic=True):
    """One text element.

    Runtime-mutable labels declare `bind`/`bind_target: text`, which is what
    generates the settable text state; a label without a bind is compiled as
    static text.
    """
    obj = {
        "type": "label",
        "name": name,
        "x": x,
        "y": y,
        "w": w,
        "h": h,
        "text": text,
        "font_size": size,
        "fg_color": color,
        "text_align": align,
        "font_charset": CHARSET,
    }
    if bold:
        obj["font"] = "fonts/DejaVuSans-Bold.ttf"
    if hidden:
        obj["hidden"] = True
    if dynamic:
        obj["bind"] = name
        obj["bind_target"] = "text"
    return obj


def build() -> dict:
    objects: list[dict] = []

    # --- header -----------------------------------------------------------
    objects.append(
        {"type": "container", "parent": -1, "name": "header", "x": 0, "y": 0,
         "w": SCREEN_W, "h": HEADER_H, "bg_color": PANEL}
    )
    objects.append(dict(label("brand", 24, 12, 220, 26, "MAGPIE", 19, ACCENT,
                              bold=True, dynamic=False),
                        parent_name="header"))
    objects.append(dict(label("transport", 232, 14, 90, 22, "", 15, DIM,
                              align="right"), parent_name="header"))
    objects.append(dict(label("status", 330, 12, 126, 26, "CONNECTING", 17,
                              DIM, align="right", bold=True),
                        parent_name="header"))
    objects.append(dict(label("tightest", 24, 40, 432, 20,
                              "waiting for first snapshot", 15, DIM),
                        parent_name="header"))

    # --- paging ----------------------------------------------------------
    # PageFlow claims horizontal drags before the scene swipe, so the pages
    # snap and settle with the framework's own gesture policy.
    objects.append(
        {"type": "page_flow", "parent": -1, "name": "pages",
         "x": 0, "y": FLOW_Y, "w": SCREEN_W, "h": FLOW_H,
         "page_count": SLOTS, "pages": [f"page{slot}" for slot in range(SLOTS)],
         "axis": "horizontal", "bar_height": 0, "selected": 0,
         "enabled": True, "bg_color": BG}
    )

    for slot in range(SLOTS):
        page = f"page{slot}"
        prefix = f"p{slot}"
        objects.append(
            {"type": "layer", "parent_name": "pages", "name": page,
             "x": slot * SCREEN_W, "y": 0, "w": SCREEN_W, "h": FLOW_H,
             "bg_color": BG, "clip_children": True}
        )
        objects.append(dict(label(f"{prefix}_name", 24, 2, 320, 38, "-", 28,
                                  INK, bold=True), parent_name=page))
        objects.append(dict(label(f"{prefix}_plan", 340, 12, 116, 22, "", 15,
                                  DIM, align="right"), parent_name=page))
        for window in range(WINDOWS):
            top = WINDOW_TOP + window * WINDOW_BLOCK_H
            # Unused window rows are hidden at startup; the firmware toggles
            # them with the container wrapper below (labels have one bind each).
            row = f"{prefix}_w{window}"
            wrap = {"type": "container", "parent_name": page, "name": row,
                    "x": 0, "y": 0, "w": SCREEN_W, "h": WINDOW_BLOCK_H,
                    "hidden": True, "bind": row, "bind_target": "visible"}
            objects.append(dict(wrap))
            objects.append(
                dict(label(f"{row}_label", 24, top, 210, 20, "", 16, DIM),
                     parent_name=row)
            )
            objects.append(
                dict(label(f"{row}_used", 24, top + 16, 180, 44, "", 36, INK,
                           bold=True), parent_name=row)
            )
            objects.append(
                dict(label(f"{row}_reset", 200, top + 30, 256, 22, "", 15, DIM,
                           align="right"), parent_name=row)
            )
            objects.append(
                {"type": "progress", "parent_name": row,
                 "name": f"{row}_bar", "x": 24, "y": top + 64, "w": 432,
                 "h": 12, "min": 0, "max": 100, "value": 0, "radius": 6,
                 "bg_color": TRACK, "fg_color": ACCENT}
            )
        error = label(f"{prefix}_error", 24, WINDOW_TOP, 432, 34, "SIGNED OUT",
                      24, ACCENT, bold=True, hidden=True)
        error["bind_target"] = "visible"
        objects.append(dict(error, parent_name=page))
        empty = label(f"{prefix}_empty", 24, WINDOW_TOP, 432, 34,
                      "No quota windows", 20, DIM, hidden=True,
                      dynamic=False)
        empty["bind"] = f"{prefix}_empty"
        empty["bind_target"] = "visible"
        objects.append(dict(empty, parent_name=page))

    # --- footer: page dots and counter -----------------------------------
    objects.append(
        {"type": "container", "parent": -1, "name": "footer", "x": 0,
         "y": FOOTER_Y, "w": SCREEN_W, "h": SCREEN_H - FOOTER_Y,
         "bg_color": PANEL}
    )
    # Two dots per slot: the plain one, plus an accent "current page" dot that
    # the firmware reveals next to it. Toggling visibility is a plain bind;
    # containers have no writable bg_color property without an extra bind.
    dot_span = SLOTS * 16
    for slot in range(SLOTS):
        x = (SCREEN_W - dot_span) // 2 + slot * 16 + 4
        objects.append(
            {"type": "container", "parent_name": "footer",
             "name": f"dot{slot}", "x": x, "y": 14, "w": 8, "h": 8,
             "radius": 4, "bg_color": DOT, "hidden": True,
             "bind": f"dot{slot}", "bind_target": "visible"}
        )
        objects.append(
            {"type": "container", "parent_name": "footer",
             "name": f"dot{slot}_on", "x": x, "y": 14, "w": 8, "h": 8,
             "radius": 4, "bg_color": ACCENT, "hidden": True,
             "bind": f"dot{slot}_on", "bind_target": "visible"}
        )
    objects.append(dict(label("counter", 372, 6, 84, 22, "", 15, DIM,
                              align="right"), parent_name="footer"))

    return {
        "screen": "magpie",
        "w": SCREEN_W,
        "h": SCREEN_H,
        "screen_bg": BG,
        "font": "fonts/DejaVuSans.ttf",
        "font_charset": CHARSET,
        "default_font_size": 16,
        # Scene swipes would fight PageFlow for the same drag; the pages widget
        # is the only pager this app wants.
        "swipe": False,
        "objects": objects,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify ui/main.json is up to date")
    args = parser.parse_args(argv)
    document = build()
    text = json.dumps(document, indent=2, ensure_ascii=False) + "\n"
    if args.check:
        current = OUTPUT.read_text() if OUTPUT.exists() else ""
        if current != text:
            print("ui/main.json is stale; run python3 ui/generate.py",
                  file=sys.stderr)
            return 1
        print("ui/main.json is up to date")
        return 0
    OUTPUT.write_text(text)
    print(f"wrote {OUTPUT} ({len(document['objects'])} objects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
