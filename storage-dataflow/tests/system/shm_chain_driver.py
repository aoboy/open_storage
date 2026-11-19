"""W6.1 process-pair driver: the Python control loop against a LIVE
wm-proxy process over the ADR-0002 segment.

The chain under test (the production topology of design 01):

    C proxy (--shm, --pace-ns, SDF_METRICS_ENDPOINT)
      | per control tick: push per-origin stats into the telemetry ring
      v
    this driver: WmControlLoop.step() -- drain telemetry, score,
      publish the new profile (version advances)
      v
    C proxy: acquires the published profile at its next tick
      v
    scrape (SDF_METRICS_ENDPOINT): sdf_proxy_shm_profile_version >= 1,
      heartbeat ticks > 0

Exits nonzero on any failure.
"""

from __future__ import annotations

import socket
import sys
import time

sys.path.insert(0, "python")

from sdf.common.shm import WmShm, unlink
from sdf.control.wm_control import WmControlLoop


def scrape(port: int) -> str:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(b"GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
    buf = b""
    while True:
        d = s.recv(4096)
        if not d:
            break
        buf += d
    s.close()
    return buf.decode()


def metric(text: str, name: str) -> int:
    for line in text.splitlines():
        if line.startswith(name + " "):
            return int(line.split()[1])
    raise AssertionError(f"metric {name!r} missing from the scrape")


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: shm_chain_driver.py NAME N_PATHS PORT TIMEOUT_S",
              file=sys.stderr)
        return 2
    name, n_paths, port, timeout_s = (
        sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))

    deadline = time.monotonic() + timeout_s
    shm = None
    while shm is None:
        try:
            shm = WmShm(name, n_paths)  # attach: the proxy created it
        except FileNotFoundError:
            if time.monotonic() > deadline:
                print("driver: the segment never appeared", file=sys.stderr)
                return 1
            time.sleep(0.05)

    loop = WmControlLoop(shm)
    versions_seen = 0
    telemetry_slots = 0

    while time.monotonic() < deadline:
        rec = loop.step()
        versions_seen = max(versions_seen, rec["version"])
        telemetry_slots += len(shm.read_telemetry())

        # the scrape carries both ends: the version we published (the
        # proxy acquires it at its next tick) and the heartbeat
        try:
            text = scrape(port)
        except (ConnectionRefusedError, OSError):
            text = ""  # between runs; the loop keeps stepping
        if text:
            acquired = metric(text, "sdf_proxy_shm_profile_version")
            ticks = metric(text, "sdf_proxy_control_heartbeat_ticks")
            pushed = metric(text, "sdf_proxy_shm_telemetry_pushed")
            if acquired >= 1 and ticks >= 1 and pushed >= n_paths:
                print(f"driver: OK version={acquired} ticks={ticks} "
                      f"pushed={pushed} steps={versions_seen} "
                      f"slots_drained={telemetry_slots}")
                return 0
        time.sleep(0.1)

    print("driver: the chain never closed in time", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
