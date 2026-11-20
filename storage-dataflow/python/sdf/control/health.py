"""Small HTTP health/readiness surface for the control-plane service."""

from __future__ import annotations

import json
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


class HealthState:
    """Thread-safe snapshot of the latest service tick."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._payload: dict[str, Any] = {"status": "starting", "heartbeat": 0}

    def update(self, record: dict[str, Any]) -> None:
        payload = {"status": "ok", **record}
        with self._lock:
            self._payload = payload

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return dict(self._payload)


class _Handler(BaseHTTPRequestHandler):
    server: _HealthHTTPServer

    def do_GET(self) -> None:
        if self.path not in ("/healthz", "/readyz"):
            self.send_error(404)
            return
        payload = self.server.state.snapshot()
        body = (json.dumps(payload, sort_keys=True) + "\n").encode()
        code = 200 if payload.get("status") == "ok" else 503
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        return


class _HealthHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], state: HealthState) -> None:
        self.state = state
        super().__init__(address, _Handler)


class HealthServer:
    """Bind ``host:port`` and serve ``/healthz`` and ``/readyz``."""

    def __init__(self, addr: str, state: HealthState) -> None:
        host, sep, port_text = addr.rpartition(":")
        if not sep or not host:
            raise ValueError("health address must be HOST:PORT")
        try:
            port = int(port_text)
        except ValueError as exc:
            raise ValueError("health port must be an integer") from exc
        if not 0 <= port <= 65535:
            raise ValueError("health port out of range")
        self._state = state
        self._server = _HealthHTTPServer((host, port), state)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def address(self) -> tuple[str, int]:
        host, port = self._server.server_address[:2]
        return str(host), int(port)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        if self._thread.is_alive():
            self._thread.join(timeout=5.0)


def parse_address(addr: str) -> tuple[str, int]:
    """Validate and split ``HOST:PORT`` (helper for callers/tests)."""
    host, sep, port_text = addr.rpartition(":")
    if not sep or not host:
        raise ValueError("address must be HOST:PORT")
    port = int(port_text)
    if not 0 <= port <= 65535:
        raise ValueError("port out of range")
    socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    return host, port
