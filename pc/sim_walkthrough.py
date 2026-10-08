#!/usr/bin/env python3
"""Drive the Dilly-dally simulator through every page and save screenshots.

Starts the GSP simulator with its automation port open, attaches the native
backend (the same dd_app.c the board runs), then taps and swipes through the
flows and checks the state after each step. Build the backend first with
`python mosaico.py project sim --project projects/dillydally --headless`.

    python sim_walkthrough.py <out_dir>
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
BUILD = PROJECT / "build-sim"
MANIFEST = json.loads((BUILD / "dillydally_backend-Release.json").read_text())
SIM = Path(os.environ.get("USERPROFILE", "~")) / ".cache/esp-mosaico/sim/1.5.1/sim.exe"
sys.path.insert(0, str(Path(MANIFEST["component_root"]) / "tools" / "sim_bridge"))
from rpc_client import connect  # noqa: E402

API_PORT = 8266
BACKEND = "tcp://127.0.0.1:9101"
W = 480

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main(out: Path) -> int:
    out.mkdir(parents=True, exist_ok=True)
    sim = subprocess.Popen([str(SIM), "--bundle", MANIFEST["bundle"], "--frames", "0",
                            "--headless", "--backend-listen", BACKEND,
                            "--api-json", str(Path(MANIFEST["bundle"]).parent / "bundle_gsp.api.json"),
                            "--api-listen", f"tcp://127.0.0.1:{API_PORT}"],
                           stdout=subprocess.DEVNULL, stderr=open(out / "sim.log", "w"))
    backend = None
    try:
        time.sleep(2)
        backend = subprocess.Popen([MANIFEST["executable"], "--endpoint", BACKEND],
                                   stderr=open(out / "backend.log", "w"))
        time.sleep(2)
        with connect(port=API_PORT) as api:
            def call(m, p=None):
                r = api.call(m, p or {})
                if "error" in r:
                    print("  rpc error", m, p, r["error"])
                return r.get("result")

            def shot(name):
                call("wait", {"frames": 3})
                call("screenshot", {"path": str(out / f"{name}.png"), "format": "png"})

            def page():
                r = call("component_get_motion", {"name": "pages"})
                return (r or {}).get("state", {}).get("value")

            def swipe_left():
                call("drag", {"x1": 420, "y1": 240, "x2": 60, "y2": 240})
                time.sleep(0.6)

            def swipe_right():
                call("drag", {"x1": 60, "y1": 240, "x2": 420, "y2": 240})
                time.sleep(0.6)

            def tap(name):
                # Named bounds are compiled page flow coordinates, so pages
                # past the first sit at x 480, 960 and on. Fold back on screen.
                b = (call("inspect_object", {"name": name}) or {}).get("bounds")
                if not b:
                    print("  no bounds for", name)
                    failures.append("bounds " + name)
                    return
                x = ((b["x1"] + b["x2"]) // 2) % W
                y = (b["y1"] + b["y2"]) // 2
                call("tap", {"x": x, "y": y})
                time.sleep(0.25)

            def visible(name):
                r = call("inspect_object", {"name": name})
                return r

            caps = call("capabilities")
            print("caps", {k: caps.get(k) for k in ("width", "height", "named_api")} if caps else caps)

            print("face")
            time.sleep(1)
            shot("01_face_idle")
            check(page() == 0, "starts on the face page")
            tap("poke")
            time.sleep(0.3)
            shot("02_face_poked")
            for _ in range(4):
                tap("poke")
            time.sleep(0.3)
            shot("03_face_poked_a_lot")

            print("clock")
            swipe_left()
            check(page() == 1, "swipe left reaches the clock")
            shot("04_clock")

            print("timer")
            swipe_left()
            check(page() == 2, "swipe left reaches the timer")
            tap("preset2")
            shot("05_timer_set_45")
            tap("t_plus")
            tap("t_minus")
            tap("t_minus")
            shot("06_timer_set_40")
            tap("t_start")
            time.sleep(2.2)
            shot("07_timer_running")
            tap("t_pause")
            time.sleep(1.5)
            shot("08_timer_paused")
            tap("t_pause")
            time.sleep(0.5)
            tap("t_stop")
            shot("09_timer_stopped")

            print("audio")
            swipe_left()
            check(page() == 3, "swipe left reaches audio")
            shot("10_audio_idle")
            tap("a_play")
            time.sleep(0.4)
            shot("11_audio_tuning")
            time.sleep(1.4)
            shot("12_audio_playing")
            tap("a_next")
            time.sleep(1.6)
            shot("13_audio_next")
            tap("tab_speech")
            time.sleep(0.4)
            shot("14_audio_speeches")
            tap("a_prev")
            shot("15_audio_speech_prev")

            print("settings")
            swipe_left()
            check(page() == 4, "swipe left reaches settings")
            shot("16_settings")
            tap("row_tz")
            shot("17_editor_tz")
            tap("ed_plus")
            shot("18_editor_tz_plus")
            # A drag on the open sheet must not move the pages.
            call("drag", {"x1": 60, "y1": 240, "x2": 420, "y2": 240})
            time.sleep(0.6)
            check(page() == 4, "drag on the open sheet leaves the page alone")
            tap("ed_minus")
            tap("ed_done")
            tap("row_auto")
            tap("ed_plus")
            tap("ed_plus")
            shot("19_editor_auto")
            tap("ed_done")
            tap("row_wifi")
            shot("20_editor_wifi")
            tap("ed_done")
            tap("tg_h24")
            time.sleep(0.3)
            shot("21_settings_after")
            tap("row_bt")
            time.sleep(0.4)
            shot("23_bt_searching")
            time.sleep(2.4)
            shot("24_bt_found")
            tap("bt_row1")
            time.sleep(1.6)
            shot("25_bt_connected")
            tap("bt_done")
            shot("26_settings_bt")

            print("back to the clock, 24 hour")
            for _ in range(3):
                swipe_right()
            check(page() == 1, "swipe right returns to the clock")
            shot("22_clock_24h")

            print("auto change, 30 s after a minute of no touch, so just check it stays put now")
            time.sleep(1)
            check(page() == 1, "no auto change while recently touched")

            call("quit")
    finally:
        for p in (backend, sim):
            if p and p.poll() is None:
                p.kill()
    print("\n%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1]) if len(sys.argv) > 1 else PROJECT / "docs" / "sim"))
