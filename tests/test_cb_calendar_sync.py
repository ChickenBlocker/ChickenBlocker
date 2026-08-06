import datetime as dt
import importlib.util
import json
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import tempfile
import threading
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "cb_calendar_sync", ROOT / "cb_calendar_sync.py"
)
calendar_sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(calendar_sync)


def khal_line(rows):
    return json.dumps(rows)


class ParseKhalOutputTests(unittest.TestCase):
    def test_filters_and_deduplicates_occurrences(self):
        timed = {
            "uid": "meeting-1",
            "start-long-full": "2026-09-17T10:00:00+0200",
            "end-long-full": "2026-09-17T11:00:00+0200",
            "all-day": "False",
            "status": "CONFIRMED",
        }
        all_day = {
            "uid": "all-day",
            "start-long-full": "2026-09-18T00:00:00+0200",
            "end-long-full": "2026-09-19T00:00:00+0200",
            "all-day": "True",
            "status": "CONFIRMED",
        }
        cancelled = {
            "uid": "cancelled",
            "start-long-full": "2026-09-19T10:00:00+0200",
            "end-long-full": "2026-09-19T11:00:00+0200",
            "all-day": "False",
            "status": "CANCELLED",
        }
        output = "\n".join(
            (khal_line([timed, all_day]), khal_line([timed, cancelled]))
        )

        events = calendar_sync.parse_khal_output(output)

        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][1], 1789632000)
        self.assertEqual(events[0][2], 1789635600)
        self.assertEqual(len(events[0][0]), 64)

    def test_rejects_malformed_or_naive_timestamps(self):
        bad = khal_line(
            [
                {
                    "uid": "bad",
                    "start-long-full": "2026-09-17T10:00:00",
                    "end-long-full": "2026-09-17T11:00:00",
                    "all-day": "False",
                    "status": "",
                }
            ]
        )
        with self.assertRaises(calendar_sync.CalendarSyncError):
            calendar_sync.parse_khal_output(bad)
        with self.assertRaises(calendar_sync.CalendarSyncError):
            calendar_sync.parse_khal_output("not json")


@unittest.skipUnless(shutil.which("khal"), "khal is not installed")
class KhalIntegrationTests(unittest.TestCase):
    def test_khal_expands_recurring_events(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            calendar_dir = base / "calendar"
            calendar_dir.mkdir()
            (calendar_dir / "events.ics").write_text(
                "\n".join(
                    (
                        "BEGIN:VCALENDAR",
                        "VERSION:2.0",
                        "PRODID:-//ChickenBlocker test//EN",
                        "BEGIN:VEVENT",
                        "UID:recurring-test",
                        "DTSTAMP:20260901T000000Z",
                        "DTSTART:20260917T100000Z",
                        "DTEND:20260917T103000Z",
                        "RRULE:FREQ=DAILY;COUNT=2",
                        "SUMMARY:Not emitted by helper",
                        "END:VEVENT",
                        "END:VCALENDAR",
                        "",
                    )
                ),
                encoding="utf-8",
            )
            config = base / "khal.conf"
            config.write_text(
                "\n".join(
                    (
                        "[calendars]",
                        "[[chickenblocker]]",
                        f"path = {calendar_dir}",
                        "readonly = True",
                        "",
                        "[locale]",
                        "local_timezone = UTC",
                        "default_timezone = UTC",
                        "timeformat = %H:%M",
                        "dateformat = %Y-%m-%d",
                        "longdateformat = %Y-%m-%d",
                        "datetimeformat = %Y-%m-%dT%H:%M:%S%z",
                        "longdatetimeformat = %Y-%m-%dT%H:%M:%S%z",
                        "",
                        "[sqlite]",
                        f"path = {base / 'khal.db'}",
                        "",
                    )
                ),
                encoding="utf-8",
            )

            events = calendar_sync.khal_snapshot(
                str(config), shutil.which("khal"), dt.date(2026, 9, 16)
            )

            self.assertEqual(len(events), 2)
            self.assertEqual(events[1][1] - events[0][1], 86400)

    @unittest.skipUnless(shutil.which("vdirsyncer"), "vdirsyncer is not installed")
    def test_vdirsyncer_and_khal_end_to_end(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            feed_dir = base / "feed"
            feed_dir.mkdir()
            (feed_dir / "calendar.ics").write_text(
                "\n".join(
                    (
                        "BEGIN:VCALENDAR",
                        "VERSION:2.0",
                        "PRODID:-//ChickenBlocker test//EN",
                        "BEGIN:VEVENT",
                        "UID:end-to-end",
                        "DTSTAMP:20260901T000000Z",
                        "DTSTART:20260920T120000Z",
                        "DTEND:20260920T130000Z",
                        "SUMMARY:Private title",
                        "END:VEVENT",
                        "END:VCALENDAR",
                        "",
                    )
                ),
                encoding="utf-8",
            )

            class QuietHandler(SimpleHTTPRequestHandler):
                def log_message(self, _format, *_args):
                    pass

            server = ThreadingHTTPServer(
                ("127.0.0.1", 0),
                lambda *args, **kwargs: QuietHandler(
                    *args, directory=str(feed_dir), **kwargs
                ),
            )
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                calendar_dir = base / "calendar"
                calendar_dir.mkdir()
                vdir_config = base / "vdirsyncer.conf"
                vdir_config.write_text(
                    "\n".join(
                        (
                            "[general]",
                            f'status_path = "{base / "status"}"',
                            "",
                            "[pair chickenblocker]",
                            'a = "calendar_local"',
                            'b = "calendar_remote"',
                            "collections = null",
                            'conflict_resolution = "b wins"',
                            'partial_sync = "revert"',
                            "",
                            "[storage calendar_local]",
                            'type = "filesystem"',
                            f'path = "{calendar_dir}"',
                            'fileext = ".ics"',
                            "",
                            "[storage calendar_remote]",
                            'type = "http"',
                            f'url = "http://127.0.0.1:{server.server_port}/calendar.ics"',
                            "",
                        )
                    ),
                    encoding="utf-8",
                )
                khal_config = base / "khal.conf"
                khal_config.write_text(
                    "\n".join(
                        (
                            "[calendars]",
                            "[[chickenblocker]]",
                            f"path = {calendar_dir}",
                            "readonly = True",
                            "",
                            "[locale]",
                            "local_timezone = UTC",
                            "default_timezone = UTC",
                            "timeformat = %H:%M",
                            "dateformat = %Y-%m-%d",
                            "longdateformat = %Y-%m-%d",
                            "datetimeformat = %Y-%m-%dT%H:%M:%S%z",
                            "longdatetimeformat = %Y-%m-%dT%H:%M:%S%z",
                            "",
                            "[sqlite]",
                            f"path = {base / 'khal.db'}",
                            "",
                        )
                    ),
                    encoding="utf-8",
                )

                events = calendar_sync.synchronize(
                    str(vdir_config),
                    str(khal_config),
                    shutil.which("vdirsyncer"),
                    shutil.which("khal"),
                )

                self.assertEqual(len(events), 1)
                self.assertEqual(events[0][2] - events[0][1], 3600)
                repeated = calendar_sync.synchronize(
                    str(vdir_config),
                    str(khal_config),
                    shutil.which("vdirsyncer"),
                    shutil.which("khal"),
                )
                self.assertEqual(repeated, events)
            finally:
                server.shutdown()
                thread.join()
                server.server_close()


if __name__ == "__main__":
    unittest.main()
