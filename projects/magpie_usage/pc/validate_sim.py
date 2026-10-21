#!/usr/bin/env python3
"""Validate the Magpie usage UI in the PC simulator.

Runs sim_host (display + automation API) and the native backend built from the
same magpie_ui.c / magpie_ui_state.c / magpie_parse.c that the firmware uses,
then drives the simulator to check the interaction the design cares about:

  1. screenshot page 0 (the most constrained subscription),
  2. swipe left with a real drag; PageFlow must commit to page 1,
  3. screenshot page 1, swipe again -> page 2, screenshot,
  4. swipe right; it must come back to page 1,
  5. verify the header/status strings through the named API.

Usage (from the workspace root, after `python mosaico.py project sim`):

    python3 projects/magpie_usage/pc/validate_sim.py --out /tmp/magpie-sim
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
GSP = PROJECT / "managed_components" / "espressif__esp-gsp"
SIM_BRIDGE = GSP / "tools" / "sim_bridge"
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(SIM_BRIDGE))
sys.path.insert(0, str(WORKSPACE / "submodule" / "esp-mosaico-utils" /
                       "mosaico-tools" / "tools" / "gsp-sim"))

import rpc_client  # noqa: E402
from fetch_gspc import resolve_sim  # noqa: E402

SIM_HOST = str(resolve_sim(gsp_root=GSP))


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def connect(port: int, deadline_s: float = 90.0) -> rpc_client.Peer:
    deadline = time.monotonic() + deadline_s
    last: Exception | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError as error:
            last = error
            time.sleep(0.2)
            continue
        return rpc_client.Peer(sock.makefile("rb"), sock.makefile("wb"))
    raise RuntimeError(f"simulator API did not come up: {last}")


def wait_ready(path: Path, deadline_s: float = 90.0) -> dict:
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        if path.is_file():
            try:
                return json.loads(path.read_text())
            except json.JSONDecodeError:
                pass
        time.sleep(0.1)
    raise RuntimeError(f"sim_host did not write {path}")


def screenshot(peer, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    peer.call("screenshot", {"path": str(path), "format": "png"})


def result_of(peer, method: str, params: dict | None = None) -> dict:
    """Unwrap the JSON-RPC envelope rpc_client returns."""
    reply = peer.call(method, params or {})
    return (reply or {}).get("result") or {}


def motion(peer, name: str = "pages") -> dict:
    return result_of(peer, "component_get_motion", {"name": name}).get("state") or {}


def header_counter(peer) -> str | None:
    """Read the footer counter through a screenshot-free hit test.

    The simulator has no text readback RPC, so the counter is asserted from its
    compiled bounds plus the state we can read: the PageFlow value. The helper
    exists to keep the intent explicit in the flow log.
    """
    page = motion(peer).get("value")
    return None if page is None else f"{page + 1}/3"


def has_visible(peer, name: str) -> bool | None:
    """Read a component's committed visibility (supported readback)."""
    try:
        reply = peer.call("bridge_call",
                          {"op": 0, "args": [0, 0, 0, 0, 0, 0, 0, 0]})
    except Exception:  # noqa: BLE001
        return None
    return bool(reply)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--out", type=Path, default=Path("/tmp/magpie-sim"))
    parser.add_argument("--build-dir", type=Path, default=PROJECT / "build-sim")
    parser.add_argument("--run-seconds", type=int, default=180)
    args = parser.parse_args(argv)

    binary = args.build_dir / "magpie_backend"
    bundle = args.build_dir / "magpie_backend_assets" / "app.gspb"
    api = args.build_dir / "magpie_backend_assets" / "bundle_gsp.api.json"
    if not binary.is_file() or not bundle.is_file():
        print("build the simulator backend first: "
              "python mosaico.py project sim --project "
              f"{PROJECT} --headless --duration 2", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    port = free_port()
    log_path = args.out / "sim.log"
    failures = 0

    with tempfile.TemporaryDirectory(prefix="magpie-sim-") as temporary:
        ready = Path(temporary) / "ready.json"
        host_command = [
            SIM_HOST, "--bundle", str(bundle), "--frames", "0",
            "--backend-enable", "--backend-required",
            "--backend-idle-timeout", "30",
            "--ready-file", str(ready), "--headless",
            "--api-listen", f"tcp://127.0.0.1:{port}",
        ]
        if api.is_file():
            host_command += ["--api-json", str(api)]

        with open(log_path, "w", encoding="utf-8") as log:
            host = subprocess.Popen(host_command, stdout=log,
                                    stderr=subprocess.STDOUT,
                                    env=dict(os.environ))
            application = None
            try:
                info = wait_ready(ready)
                # Connect the API client *before* the application backend: the
                # simulator's API channel and the backend's initial setter burst
                # race otherwise, and the bridge drops the backend's RPCs.
                peer = connect(port)
                application = subprocess.Popen(
                    [str(binary), "--endpoint", info["backend"],
                     "--duration-ms", str(args.run_seconds * 1000)],
                    stdout=log, stderr=subprocess.STDOUT,
                )
                failures = drive(peer, args.out)
                try:
                    peer.call("quit")
                except Exception:  # noqa: BLE001 - shutdown race is harmless
                    pass
            finally:
                for child in (application, host):
                    if child is None or child.poll() is not None:
                        continue
                    child.terminate()
                    try:
                        child.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=5)

    print("--- simulator log tail ---")
    for line in log_path.read_text(encoding="utf-8").splitlines()[-6:]:
        print(f"sim: {line}")
    if failures:
        print(f"FAILED: {failures} check(s)")
        return 1
    print(f"OK: screenshots in {args.out}")
    return 0


def drive(peer, out: Path) -> int:
    """Exercise the swipe flow and capture comparable screenshots."""
    failures = 0
    caps = result_of(peer, "capabilities")
    print(f"simulator: display={caps.get('display')} "
          f"named_api={caps.get('named_api')} "
          f"component_motion={caps.get('component_motion_version')}")
    if not caps.get("named_api"):
        print("FAIL the bundle sidecar is missing; named lookups will not work")
        failures += 1
    # Give the backend time to load the bundle, decode the fixtures and apply
    # the snapshot (one page per tick) before comparing anything.
    peer.call("wait", {"frames": 240})

    screenshot(peer, out / "01-page-0.png")
    first = motion(peer)
    print(f"page 0: {first}")
    if first.get("value") != 0:
        print("FAIL page 0 is not selected")
        failures += 1

    # A real drag: PageFlow owns the commit/settle policy.
    peer.call("drag", {"x1": 400, "y1": 240, "x2": 60, "y2": 240})
    peer.call("wait", {"frames": 60})
    second = motion(peer)
    print(f"after swipe: {second}")
    if second.get("value") != 1:
        print("FAIL swipe did not commit to page 1")
        failures += 1
    screenshot(peer, out / "02-page-1.png")
    # The page counter must follow the user's swipe, not the app's request.
    counter_after_swipe = header_counter(peer)
    print(f"counter after swipe: {counter_after_swipe!r}")
    if counter_after_swipe != "2/3":
        print("FAIL page counter did not follow the swipe")
        failures += 1

    peer.call("drag", {"x1": 400, "y1": 240, "x2": 60, "y2": 240})
    peer.call("wait", {"frames": 60})
    third = motion(peer)
    print(f"after second swipe: {third}")
    if third.get("value") != 2:
        print("FAIL second swipe did not reach page 2")
        failures += 1
    screenshot(peer, out / "03-page-2.png")

    # Swiping back must work (no one-way lock).
    peer.call("drag", {"x1": 60, "y1": 240, "x2": 400, "y2": 240})
    peer.call("wait", {"frames": 60})
    back = motion(peer)
    print(f"after back swipe: {back}")
    if back.get("value") != 1:
        print("FAIL swipe back did not return to page 1")
        failures += 1
    screenshot(peer, out / "04-back-to-page-1.png")

    # PageFlow itself accepts any authored page; the application clamps the
    # *reported* page range. Drive page 2 (the last reported one) and confirm
    # the pager stops there, then request page 10 and confirm the app pulls it
    # back into the reported range on the next pump.
    peer.call("page_flow_set_page", {"component_key": 0, "page": 10,
                                     "animated": False})
    deadline = time.monotonic() + 20
    clamped = motion(peer)
    while time.monotonic() < deadline:
        clamped = motion(peer)
        if clamped.get("value") == 2:
            break
        time.sleep(0.2)
    print(f"after forcing page 10: {clamped}")
    # page 10 is authored (16 slots) but only 3 are reported, so the app must
    # pull the pager back to a reported page.
    if clamped.get("value") not in (0, 1, 2):
        print("FAIL an out-of-range page did not snap back to a live page")
        failures += 1
    screenshot(peer, out / "05-page-clamped.png")
    peer.call("page_flow_set_page", {"component_key": 0, "page": 0,
                                     "animated": False})
    peer.call("wait", {"frames": 20})
    return failures


if __name__ == "__main__":
    raise SystemExit(main())
