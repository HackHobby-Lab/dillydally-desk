#!/usr/bin/env python3
"""Generate ui/main.json, ui/main_sim.json and main/dd_faces_gen.h.

One scene. A horizontal page flow holds the five pages, so swiping is the page
flow's own gesture and the status bar and the two sheets sit outside it and
stay put. Faces are baked images, see face_art.py; every face
appears as an open and a blink image and C flips visibility between them.

The face tables C needs are emitted from here so the scene and the code cannot
disagree about which images exist.

Run from this directory:  python face_art.py && python build_scene.py
"""

import json

from face_art import FACES

W = H = 480

INK = "#000000"   # true black, the panel is AMOLED
CARD = "#201F1D"
CARD_HI = "#2B2926"
LINE = "#2E2C29"
PAPER = "#F0EEE6"
DIM = "#8A877F"
FAINT = "#5A5752"
ACCENT = "#FF4C01"
CLEAR = "#00000000"

BOLD = "fonts/DejaVuSans-Bold.ttf"
# Seven segment digits, fixed width, so "12:23" and "88:88" measure the same
# and nothing beside the time can be overlapped by it. DSEG, SIL OFL 1.1.
SEG = "fonts/DSEG7Classic-Bold.ttf"
GHOST = "#151515"  # unlit segments behind the time
DIGITS = "0123456789:"
ASCII = (" !\"#$%&'()*+,-./0123456789:;<=>?@"
         "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
         "abcdefghijklmnopqrstuvwxyz{|}~")

PAGES = ["p_face", "p_clock", "p_timer", "p_audio", "p_settings"]

objects = []


def add(**o):
    if "parent" not in o and "parent_name" not in o:
        o["parent"] = -1
    objects.append(o)
    return o.get("name")


def label(parent, x, y, w, h, text, size, color, name=None, bold=False,
          align="left", charset=None, font=None):
    o = dict(type="label", parent_name=parent, x=x, y=y, w=w, h=h, text=text,
             font_size=size, fg_color=color, text_align=align)
    if font:
        o["font"] = font
    elif bold:
        o["font"] = BOLD
    if name:
        o["name"] = name
        o["bind"] = name
        o["overflow"] = "ellipsis" if align != "center" or w < 400 else "clip"
        if charset:
            o["font_charset"] = charset
    add(**o)


def button(parent, name, callback, x, y, w, h, radius, bg, color_bind=False):
    o = dict(type="button", parent_name=parent, name=name, callback=callback,
             x=x, y=y, w=w, h=h, radius=radius, bg_color=bg)
    if color_bind:
        o["bind"] = name + "_bg"
        o["bind_target"] = "color"
    add(**o)


def image(parent, path, x, y, w, h, name=None, hidden=False, codec=None):
    o = dict(type="image", parent_name=parent, image=path, x=x, y=y, w=w, h=h)
    if codec:
        # Faces are opaque soft gradients on the page colour, which JPEG
        # holds at a tenth of the raw size. Raw they come to 6.8 MB and the
        # ui_apps partition is 1 MB.
        o["codec"] = codec
        o["quality"] = 82
    if name:
        o["name"] = name
        o["bind"] = name
        o["bind_target"] = "visible"
        o["hidden"] = hidden
    add(**o)


# The board decodes JPEG in hardware with a colour range mismatch, so it gets
# pre-compensated faces; the simulator decodes in software and gets the true
# ones. Two scene files, identical apart from that directory. See face_art.py.
FACE_DIR = "faces_hw"

# Face sets. Each is one place on screen where a face lives, holding an open
# and a blink image per face it can show.
SETS = {}


def face_set(set_name, parent, size, x, y, keys=None):
    edge = {"l": 360, "m": 168, "s": 104}[size]
    keys = keys or [k for k, _, _ in FACES]
    entries = []
    for key in keys:
        for blink in (False, True):
            name = f"f{set_name}_{key}{'_b' if blink else ''}"
            path = f"{FACE_DIR}/{key}_{size}{'_b' if blink else ''}.png"
            image(parent, path, x, y, edge, edge, name=name,
                  hidden=not (key == "blip" and not blink), codec="jpeg")
        entries.append(key)
    SETS[set_name] = entries


# ------------------------------------------------------------- page flow
add(type="page_flow", name="pages", x=0, y=0, w=W, h=H,
    page_count=len(PAGES), selected=0, axis="horizontal", cyclic=False,
    bar_height=0, pages=PAGES, enabled=True)
for i, p in enumerate(PAGES):
    add(type="layer", parent_name="pages", name=p, x=i * W, y=0, w=W, h=H,
        bg_color=INK)

# ------------------------------------------------------------- face page
# The face bobs, so it lives in a box whose y is a runtime property.
add(type="container", parent_name="p_face", name="face_box", x=60,
    y={"default": 56, "min": 36, "max": 76}, w=360, h=360, bg_color=CLEAR)
face_set("l", "face_box", "l", 0, 0)
button("p_face", "poke", "poke", 90, 90, 300, 300, 150, CLEAR)

# ------------------------------------------------------------ clock page
# 112 px DSEG is 390 px for "88:88". The unlit segments sit behind the lit
# ones, and AM/PM rides above the right end so it can never touch a digit.
# "!" is DSEG's blank digit, used for the empty tens of a 12 hour clock.
TX = (W - 390) // 2
label("p_clock", TX, 96, 390, 124, "88:88", 112, GHOST, font=SEG)
label("p_clock", TX, 96, 390, 124, "10:42", 112, PAPER, name="clock_time",
      align="left", charset=DIGITS + "!-", font=SEG)
label("p_clock", TX + 290, 66, 100, 22, " ", 16, ACCENT, name="clock_ampm",
      bold=True, align="right", charset="AMP ")
# Seconds as a hairline that fills across the minute.
add(type="progress", parent_name="p_clock", name="clock_sec", x=TX, y=236,
    w=390, h=3, value=0, bg_color="#1C1C1C", fg_color=ACCENT, radius=2)
label("p_clock", 0, 262, W, 28, "THURSDAY", 20, ACCENT, name="clock_day",
      bold=True, align="center", charset=" ADEFHIMNORSTUWY")
label("p_clock", 0, 294, W, 30, "8 October 2026", 22, DIM, name="clock_date",
      align="center", charset=ASCII)
face_set("c", "p_clock", "s", 188, 346)

# ------------------------------------------------------------ timer page
add(type="container", parent_name="p_timer", name="t_set", bind="t_set",
    bind_target="visible", x=0, y=0, w=W, h=H, bg_color=CLEAR)
label("t_set", 0, 62, W, 28, "Focus for", 20, DIM, align="center")
for i, m in enumerate((15, 25, 45, 60)):
    x = 45 + i * 102
    button("t_set", f"preset{i}", f"preset{i}", x, 120, 84, 48, 24,
           ACCENT if m == 25 else CARD, color_bind=True)
    label("t_set", x, 131, 84, 26, str(m), 22, PAPER, bold=True, align="center")
button("t_set", "t_minus", "t_minus", 28, 220, 60, 60, 30, CARD)
label("t_set", 28, 230, 60, 40, "-", 34, PAPER, align="center")
button("t_set", "t_plus", "t_plus", 392, 220, 60, 60, 30, CARD)
label("t_set", 392, 230, 60, 40, "+", 34, PAPER, align="center")
label("t_set", 90, 214, 300, 72, "25:00", 60, PAPER, name="t_set_time",
      align="center", charset=DIGITS, font=SEG)
button("t_set", "t_start", "t_start", 140, 360, 200, 56, 28, ACCENT)
label("t_set", 140, 374, 200, 28, "Start focus", 20, PAPER, bold=True, align="center")

add(type="container", parent_name="p_timer", name="t_run", bind="t_run",
    bind_target="visible", hidden=True, x=0, y=0, w=W, h=H, bg_color=CLEAR)
add(type="arc", parent_name="t_run", name="t_ring", x=100, y=70, w=280, h=280,
    min=0, max=1000, value=1000, start_angle=0, sweep=360, thickness=14,
    bg_color=LINE, fg_color=ACCENT, interactive=False)
face_set("t", "t_run", "s", 188, 102)
label("t_run", 100, 206, 280, 62, "25:00", 52, PAPER, name="t_run_time",
      align="center", charset=DIGITS, font=SEG)
label("t_run", 100, 270, 280, 22, "of 25 min focus", 16, DIM, name="t_run_sub",
      align="center", charset=DIGITS + " ofminucsPade")
button("t_run", "t_pause", "t_pause", 96, 384, 132, 48, 24, CARD_HI)
label("t_run", 96, 396, 132, 26, "Pause", 18, PAPER, name="t_pause_text",
      bold=True, align="center", charset="PauseRm")
button("t_run", "t_stop", "t_stop", 252, 384, 132, 48, 24, CARD)
label("t_run", 252, 396, 132, 26, "Stop", 18, DIM, bold=True, align="center")

# ------------------------------------------------------------ audio page
button("p_audio", "tab_radio", "tab_radio", 110, 56, 128, 36, 18, CARD_HI, color_bind=True)
label("p_audio", 110, 64, 128, 22, "Radio", 15, PAPER, bold=True, align="center")
button("p_audio", "tab_speech", "tab_speech", 242, 56, 128, 36, 18, INK, color_bind=True)
label("p_audio", 242, 64, 128, 22, "Speeches", 15, PAPER, bold=True, align="center")
face_set("a", "p_audio", "m", 156, 104,
         keys=["blip", "sparkle", "ick", "wobble", "meh"])
label("p_audio", 20, 270, 440, 36, "Groove Salad", 28, PAPER, name="a_title",
      bold=True, align="center", charset=ASCII)
label("p_audio", 20, 308, 440, 22, "Radio  1 of 9", 16, DIM, name="a_sub",
      align="center", charset=ASCII)
button("p_audio", "a_prev", "a_prev", 108, 350, 64, 64, 32, CLEAR)
image("p_audio", "icons/prev.png", 122, 364, 36, 36)
button("p_audio", "a_play", "a_play", 202, 344, 76, 76, 38, ACCENT)
image("p_audio", "icons/play.png", 220, 362, 40, 40, name="a_icon_play")
image("p_audio", "icons/pause.png", 220, 362, 40, 40, name="a_icon_pause", hidden=True)
button("p_audio", "a_next", "a_next", 308, 350, 64, 64, 32, CLEAR)
image("p_audio", "icons/next.png", 322, 364, 36, 36)

# --------------------------------------------------------- settings page
label("p_settings", 28, 22, 300, 36, "Settings", 30, PAPER, bold=True)
RH = 46


def card(top, rows):
    add(type="container", parent_name="p_settings", x=20, y=top, w=440,
        h=RH * len(rows), bg_color=CARD, radius=18)
    for i, (key, chip, text, kind) in enumerate(rows):
        y = top + i * RH
        if i:
            add(type="container", parent_name="p_settings", x=64, y=y, w=396,
                h=1, bg_color=LINE)
        if kind == "value":
            button("p_settings", "row_" + key, "row_" + key, 20, y, 440, RH,
                   18 if (i == 0 or i == len(rows) - 1) else 0, CLEAR)
        image("p_settings", f"icons/chip_{chip}.png", 34, y + 9, 28, 28)
        label("p_settings", 78, y + 12, 210, 24, text, 18, PAPER)
        if kind == "value":
            # No chevron. The whole row is the button, and the value sits
            # flush with the card's right edge where the toggles end.
            label("p_settings", 248, y + 13, 196, 22, " ", 17, DIM,
                  name="val_" + key, align="right", charset=ASCII)
        else:
            add(type="toggle", parent_name="p_settings", name="tg_" + key,
                callback="tg_" + key, x=392, y=y + 8, w=52, h=30,
                checked=True, track_size=28, bg_color="#3A3835",
                fg_color=ACCENT, knob_color=PAPER)
    return top + RH * len(rows) + 12


top = card(68, [("wifi", "wifi", "Wi-Fi", "value"),
                ("bt", "bt", "Bluetooth audio", "value"),
                ("tz", "globe", "Time zone", "value")])
top = card(top, [("auto", "cycle", "Auto change page", "value"),
                 ("h24", "clock", "24 hour clock", "toggle"),
                 ("bright", "sun", "Brightness", "value")])
top = card(top, [("vol", "speaker", "Volume", "value"),
                 ("chime", "bell", "Chime when done", "toggle")])

# ------------------------------------------------------- fixed: status
add(type="container", name="status", bind="status", bind_target="visible",
    x=0, y=0, w=W, h=56, bg_color=CLEAR)
label("status", 24, 18, 120, 22, "10:42", 16, DIM, name="status_time",
      charset=DIGITS + "AMP ")
image("status", "icons/wifi.png", 434, 18, 24, 24, name="status_wifi", hidden=True)
image("status", "icons/wifi_off.png", 434, 18, 24, 24, name="status_wifi_off")


# ---------------------------------------------- fixed: the value editor
# One sheet edits every number in Settings: a title, the value, minus, plus.
add(type="layer", name="editor", bind="editor", bind_target="visible",
    hidden=True, x=0, y=0, w=W, h=H, bg_color="#000000B8",
    block_scene_swipe=True)
button("editor", "ed_scrim", "ed_done", 0, 0, W, H, 0, CLEAR)
add(type="container", parent_name="editor", x=40, y=110, w=400, h=260,
    bg_color=CARD, radius=24)
label("editor", 40, 132, 400, 26, "Time zone", 20, DIM, name="ed_title",
      align="center", charset=ASCII)
# Numbers: the value between minus and plus.
add(type="container", parent_name="editor", name="ed_adjust", bind="ed_adjust",
    bind_target="visible", x=40, y=160, w=400, h=110, bg_color=CLEAR)
label("ed_adjust", 92, 16, 216, 64, "UTC+5", 34, PAPER, name="ed_value",
      bold=True, align="center", charset=ASCII)
button("ed_adjust", "ed_minus", "ed_minus", 24, 16, 64, 64, 32, CARD_HI)
label("ed_adjust", 24, 28, 64, 40, "-", 34, PAPER, align="center")
button("ed_adjust", "ed_plus", "ed_plus", 312, 16, 64, 64, 32, CARD_HI)
label("ed_adjust", 312, 28, 64, 40, "+", 34, PAPER, align="center")
# Wi-Fi: what to join and where to go, while the setup network is up.
add(type="container", parent_name="editor", name="ed_info", bind="ed_info",
    bind_target="visible", hidden=True, x=40, y=160, w=400, h=120, bg_color=CLEAR)
for i in range(3):
    label("ed_info", 24, 6 + i * 36, 352, 28, "", 18, PAPER, name=f"ed_line{i}",
          align="center", charset=ASCII)
button("editor", "ed_done", "ed_done", 150, 294, 180, 52, 26, ACCENT)
label("editor", 150, 307, 180, 26, "Done", 19, PAPER, bold=True, align="center")

# ------------------------------------------------- fixed: Bluetooth sheet
# Found devices in up to four rows, a status line, Search or Disconnect, Done.
BT_ROWS = 4
add(type="layer", name="bt_sheet", bind="bt_sheet", bind_target="visible",
    hidden=True, x=0, y=0, w=W, h=H, bg_color="#000000B8",
    block_scene_swipe=True)
button("bt_sheet", "bt_scrim", "bt_done", 0, 0, W, H, 0, CLEAR)
add(type="container", parent_name="bt_sheet", x=30, y=50, w=420, h=380,
    bg_color=CARD, radius=24)
label("bt_sheet", 30, 70, 420, 26, "Bluetooth audio", 20, PAPER, bold=True,
      align="center")
label("bt_sheet", 46, 102, 388, 22, "Tap Search to find speakers", 15, DIM,
      name="bt_status", align="center", charset=ASCII)
for i in range(BT_ROWS):
    y = 134 + i * 54
    add(type="container", parent_name="bt_sheet", name=f"bt_item{i}",
        bind=f"bt_item{i}", bind_target="visible", hidden=True,
        x=46, y=y, w=388, h=46, bg_color=CLEAR)
    button(f"bt_item{i}", f"bt_row{i}", f"bt_row{i}", 0, 0, 388, 46, 14, CARD_HI)
    label(f"bt_item{i}", 16, 12, 260, 24, " ", 17, PAPER, name=f"bt_name{i}",
          charset=ASCII)
    label(f"bt_item{i}", 268, 14, 106, 20, " ", 13, ACCENT, name=f"bt_tag{i}",
          bold=True, align="right", charset=ASCII)
button("bt_sheet", "bt_scan", "bt_scan", 46, 360, 182, 50, 25, CARD_HI)
label("bt_sheet", 46, 373, 182, 26, "Search", 18, PAPER, name="bt_scan_text",
      bold=True, align="center", charset=ASCII)
button("bt_sheet", "bt_done", "bt_done", 252, 360, 182, 50, 25, ACCENT)
label("bt_sheet", 252, 373, 182, 26, "Done", 18, PAPER, bold=True, align="center")

scene = {
    "screen": "dd",
    "w": W,
    "h": H,
    "screen_bg": INK,
    "font": "fonts/DejaVuSans.ttf",
    "default_font_size": 18,
    "swipe": False,
    "objects": objects,
}

with open("main.json", "w", encoding="utf-8") as f:
    json.dump(scene, f, indent=2)
    f.write("\n")

sim_text = json.dumps(scene, indent=2).replace('"faces_hw/', '"faces/')
with open("main_sim.json", "w", encoding="utf-8") as f:
    f.write(sim_text + "\n")

# ------------------------------------------------------- C face tables
mood_index = {k: i for i, (k, _, _) in enumerate(FACES)}
out = ["// Generated by ui/build_scene.py. Do not edit.", "#pragma once", "",
       '#include "dd_face.h"', '#include "bundle_gsp.h"', ""]
for set_name, keys in SETS.items():
    out.append(f"static const dd_face_img_t dd_faces_{set_name}[] = {{")
    for key in keys:
        up = f"F{set_name}_{key}".upper()
        out.append(f"    {{ {mood_index[key]}, GSP_DD_OBJ_KEY_{up}, "
                   f"GSP_DD_OBJ_KEY_{up}_B }},")
    out.append("};")
    out.append("")
with open("../main/dd_faces_gen.h", "w", encoding="utf-8") as f:
    f.write("\n".join(out))

print("wrote main.json with %d objects" % len(objects))
