#!/usr/bin/env python3
"""Fetch recent wm-control-bench artifacts for the nightly regression gate.

The nightly job uploads out/bench/ as the `wm-control-bench` artifact.
This script queries the GitHub Actions artifacts API for the last N days,
downloads each zip, and writes its wm_control_microbench.json into
--out. The current run is excluded by --exclude-run so a night's result is
never compared against itself.

Usage:
    GITHUB_TOKEN=... fetch_bench_history.py --repo OWNER/REPO \
        --days 7 --out bench-history [--exclude-run RUN_ID]

    fetch_bench_history.py --selftest
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import urllib.error
import urllib.request
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

API = "https://api.github.com"
BENCH_FILE = "wm_control_microbench.json"
PER_PAGE = 100


def parse_ts(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def select_artifacts(
    artifacts: list[dict], cutoff: datetime, exclude_run: str | None
) -> list[dict]:
    selected = []
    for art in artifacts:
        if art.get("expired"):
            continue
        created = parse_ts(art["created_at"])
        if created < cutoff:
            continue
        run_id = str(art.get("workflow_run", {}).get("id", ""))
        if exclude_run is not None and run_id == str(exclude_run):
            continue
        selected.append(art)
    return selected


def extract_bench(blob: bytes) -> bytes:
    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        names = [n for n in zf.namelist() if n.endswith(BENCH_FILE)]
        if not names:
            raise ValueError(f"artifact contains no {BENCH_FILE}")
        return zf.read(names[0])


def api_json(url: str, token: str) -> dict:
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "sdf-bench-history",
        },
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.load(resp)


def download(url: str, token: str) -> bytes:
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "sdf-bench-history",
        },
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def list_artifacts(repo: str, token: str, name: str) -> list[dict]:
    collected: list[dict] = []
    page = 1
    while True:
        url = (
            f"{API}/repos/{repo}/actions/artifacts"
            f"?name={name}&per_page={PER_PAGE}&page={page}"
        )
        payload = api_json(url, token)
        batch = payload.get("artifacts", [])
        collected.extend(batch)
        if len(batch) < PER_PAGE:
            break
        page += 1
    return collected


def selftest() -> int:
    now = datetime.now(timezone.utc)
    fresh = (now - timedelta(days=1)).isoformat().replace("+00:00", "Z")
    old = (now - timedelta(days=9)).isoformat().replace("+00:00", "Z")
    artifacts = [
        {
            "id": 1,
            "created_at": fresh,
            "expired": False,
            "workflow_run": {"id": 10},
            "archive_download_url": "https://example.invalid/1",
        },
        {
            "id": 2,
            "created_at": old,
            "expired": False,
            "workflow_run": {"id": 11},
            "archive_download_url": "https://example.invalid/2",
        },
        {
            "id": 3,
            "created_at": fresh,
            "expired": True,
            "workflow_run": {"id": 12},
            "archive_download_url": "https://example.invalid/3",
        },
        {
            "id": 4,
            "created_at": fresh,
            "expired": False,
            "workflow_run": {"id": 99},
            "archive_download_url": "https://example.invalid/4",
        },
    ]
    chosen = select_artifacts(artifacts, now - timedelta(days=7), exclude_run="99")
    if [a["id"] for a in chosen] != [1]:
        print("selftest FAIL: artifact selection", file=sys.stderr)
        return 1

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr(f"out/bench/{BENCH_FILE}", '{"step_ns": {"p99": 123}}')
    doc = json.loads(extract_bench(buf.getvalue()))
    if doc["step_ns"]["p99"] != 123:
        print("selftest FAIL: zip extraction", file=sys.stderr)
        return 1

    print("fetch_bench_history: selftest ok (window/expiry/exclusion/extraction)")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", help="OWNER/REPO")
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--out", type=Path, default=Path("bench-history"))
    ap.add_argument("--name", default="wm-control-bench")
    ap.add_argument("--exclude-run", default=None)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()
    if not args.repo:
        ap.error("--repo is required")

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not token:
        print("fetch_bench_history: GITHUB_TOKEN/GH_TOKEN not set", file=sys.stderr)
        return 1

    cutoff = datetime.now(timezone.utc) - timedelta(days=args.days)
    try:
        artifacts = list_artifacts(args.repo, token, args.name)
        selected = select_artifacts(artifacts, cutoff, args.exclude_run)
    except (urllib.error.URLError, ValueError, KeyError) as exc:
        print(f"fetch_bench_history: API error: {exc}", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    written = 0
    for art in selected:
        run_id = art.get("workflow_run", {}).get("id", "run")
        try:
            payload = extract_bench(download(art["archive_download_url"], token))
        except (urllib.error.URLError, ValueError, zipfile.BadZipFile) as exc:
            print(f"fetch_bench_history: artifact {art.get('id')}: {exc}", file=sys.stderr)
            return 1
        path = args.out / f"{run_id}-{art['id']}.json"
        path.write_bytes(payload)
        written += 1

    print(
        f"fetch_bench_history: {written} artifact(s) from the last "
        f"{args.days} day(s) -> {args.out}"
    )
    if written == 0:
        print(
            "fetch_bench_history: no history yet; the regression gate will "
            "skip until the baseline has samples",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
