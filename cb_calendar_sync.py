#!/usr/bin/env python3
"""Synchronize the dedicated meeting calendar and emit sanitized occurrences."""

import datetime as dt
import hashlib
import json
import os
import subprocess
import sys


CONFIG_DIR = os.path.expanduser("~/.config/chickenblocker")
VDIRSYNCER_CONFIG = os.path.join(CONFIG_DIR, "vdirsyncer.conf")
KHAL_CONFIG = os.path.join(CONFIG_DIR, "khal.conf")
VDIRSYNCER_BIN = "/usr/bin/vdirsyncer"
KHAL_BIN = "/usr/bin/khal"
SYNC_TIMEOUT_SECONDS = 45
LIST_TIMEOUT_SECONDS = 30
KHAL_FIELDS = (
    "uid",
    "start-long-full",
    "end-long-full",
    "all-day",
    "status",
)


class CalendarSyncError(RuntimeError):
    pass


def _run(command, timeout):
    try:
        return subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CalendarSyncError("calendar command failed") from exc


def _parse_datetime(value):
    if not isinstance(value, str):
        raise CalendarSyncError("calendar returned a non-string timestamp")
    try:
        parsed = dt.datetime.fromisoformat(value)
    except ValueError as exc:
        raise CalendarSyncError("calendar returned an invalid timestamp") from exc
    if parsed.tzinfo is None:
        raise CalendarSyncError("calendar returned a timestamp without timezone")
    return int(parsed.timestamp())


def parse_khal_output(output):
    """Return sorted (hash, start_epoch, end_epoch) tuples from khal JSON."""
    occurrences = {}
    for line in output.splitlines():
        if not line.strip():
            continue
        try:
            rows = json.loads(line)
        except json.JSONDecodeError as exc:
            raise CalendarSyncError("khal returned malformed JSON") from exc
        if not isinstance(rows, list):
            raise CalendarSyncError("khal returned a non-list JSON value")
        for row in rows:
            if not isinstance(row, dict):
                raise CalendarSyncError("khal returned a non-object event")
            if str(row.get("all-day", "")).lower() == "true":
                continue
            if str(row.get("status", "")).strip().upper() == "CANCELLED":
                continue
            uid = row.get("uid")
            if not isinstance(uid, str) or not uid:
                raise CalendarSyncError("khal returned an event without UID")
            start = _parse_datetime(row.get("start-long-full"))
            end = _parse_datetime(row.get("end-long-full"))
            if end <= start:
                raise CalendarSyncError("khal returned a non-positive event duration")
            identity = f"{uid}\0{start}\0{end}".encode("utf-8")
            occurrence_id = hashlib.sha256(identity).hexdigest()
            occurrences[occurrence_id] = (occurrence_id, start, end)
    return sorted(occurrences.values(), key=lambda item: (item[1], item[2], item[0]))


def khal_snapshot(config_path=KHAL_CONFIG, khal_bin=KHAL_BIN, today=None):
    if today is None:
        today = dt.date.today()
    start = (today - dt.timedelta(days=1)).isoformat()
    command = [khal_bin, "--config", config_path, "list"]
    for field in KHAL_FIELDS:
        command.extend(("--json", field))
    command.extend((start, "33d"))
    result = _run(command, LIST_TIMEOUT_SECONDS)
    if result.returncode != 0:
        raise CalendarSyncError("khal could not read the synchronized calendar")
    return parse_khal_output(result.stdout)


def synchronize(
    vdirsyncer_config=VDIRSYNCER_CONFIG,
    khal_config=KHAL_CONFIG,
    vdirsyncer_bin=VDIRSYNCER_BIN,
    khal_bin=KHAL_BIN,
):
    if not os.path.isfile(vdirsyncer_config) or not os.path.isfile(khal_config):
        raise CalendarSyncError("calendar is not configured")
    result = _run(
        [vdirsyncer_bin, "--config", vdirsyncer_config, "discover", "chickenblocker"],
        SYNC_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        raise CalendarSyncError("vdirsyncer could not discover the calendar")
    result = _run(
        [vdirsyncer_bin, "--config", vdirsyncer_config, "sync", "chickenblocker"],
        SYNC_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        raise CalendarSyncError("vdirsyncer could not refresh the calendar")
    return khal_snapshot(khal_config, khal_bin)


def main():
    try:
        occurrences = synchronize()
    except CalendarSyncError as exc:
        print(f"cb_calendar_sync: {exc}", file=sys.stderr)
        return 1

    print("OK")
    for occurrence_id, start, end in occurrences:
        print(f"EVENT {occurrence_id} {start} {end}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
