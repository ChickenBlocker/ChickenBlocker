import datetime
import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.plugins.trayicon import plugin as tray  # noqa: E402


class _TmpSchedule:
    def __init__(self, content, age=0.0):
        self.path = Path("/tmp/kilo_cb_se_schedule_test") if os.access(
            "/tmp", os.W_OK) else Path("cb_se_schedule_test")
        self.content = content
        self.age = age

    def __enter__(self):
        self.path.write_text(self.content)
        old = os.utime(self.path, (time.time() - self.age,) * 2)
        assert old is None
        self._saved = tray.CB_SCHEDULE_FILE
        tray.CB_SCHEDULE_FILE = str(self.path)
        return self.path

    def __exit__(self, *exc):
        tray.CB_SCHEDULE_FILE = self._saved
        self.path.unlink(missing_ok=True)


def _fake_tray(active=True):
    return SimpleNamespace(
        active=active,
        has_breaks=lambda: True,
        date_time=None,  # upstream path unusable -> forces daemon path
        get_break_time=Mock(return_value=None),
    )


class DaemonScheduleReaderTests(TestCase):
    def test_parses_fields(self):
        with _TmpSchedule("night=0\nnext_break=100\nnext_long=400\n"
                          "interval=3\ntotal=4\n"):
            sched = tray._cb_read_daemon_schedule()
        self.assertEqual(sched, {"night": 0, "next_break": 100,
                                 "next_long": 400, "interval": 3, "total": 4})

    def test_missing_file_is_none(self):
        tray.CB_SCHEDULE_FILE = "/tmp/definitely-not-here-cb-schedule"
        self.assertIsNone(tray._cb_read_daemon_schedule())

    def test_stale_file_is_none(self):
        with _TmpSchedule("night=0\n", age=tray.CB_SCHEDULE_STALE_SECS + 60):
            self.assertIsNone(tray._cb_read_daemon_schedule())

    def test_garbage_value_is_none(self):
        with _TmpSchedule("night=soon\n"):
            self.assertIsNone(tray._cb_read_daemon_schedule())


class TrayDisplayTests(TestCase):
    def test_interval_uses_daemon_cycle(self):
        # Regression: with the neutered config the tray computed
        # ceil(10080/10080) = "1/1". It must show the daemon's X/4.
        fake = _fake_tray()
        with _TmpSchedule("night=0\nnext_break=100\nnext_long=400\n"
                          "interval=4\ntotal=4\n"):
            self.assertEqual(tray.TrayIcon.get_interval_progress(fake), (4, 4))
        with _TmpSchedule("night=0\nnext_break=100\nnext_long=400\n"
                          "interval=1\ntotal=4\n"):
            self.assertEqual(tray.TrayIcon.get_interval_progress(fake), (1, 4))

    def test_interval_hidden_at_night(self):
        fake = _fake_tray()
        with _TmpSchedule("night=1\nnext_break=100\nnext_long=100\n"
                          "interval=0\ntotal=4\n"):
            self.assertIsNone(tray.TrayIcon.get_interval_progress(fake))

    def test_next_break_times_from_daemon(self):
        fake = _fake_tray()
        nb = datetime.datetime.now().timestamp() + 1800
        nl = datetime.datetime.now().timestamp() + 7200
        with _TmpSchedule(f"night=0\nnext_break={int(nb)}\nnext_long={int(nl)}\n"
                          "interval=1\ntotal=4\n"):
            (short, long_, is_long) = tray.TrayIcon.get_next_break_time(fake)
        self.assertFalse(is_long)
        self.assertEqual(short, tray.utility.format_time(
            datetime.datetime.fromtimestamp(nb)))
        self.assertEqual(long_, tray.utility.format_time(
            datetime.datetime.fromtimestamp(nl)))

    def test_upcoming_long_break_flagged(self):
        fake = _fake_tray()
        t = datetime.datetime.now().timestamp() + 1800
        with _TmpSchedule(f"night=0\nnext_break={int(t)}\nnext_long={int(t)}\n"
                          "interval=4\ntotal=4\n"):
            (short, long_, is_long) = tray.TrayIcon.get_next_break_time(fake)
        self.assertTrue(is_long)
        self.assertEqual(short, long_)

    def test_gate_shows_no_break_time(self):
        # next_break=0 (Start-work gate / disarmed): no scheduled time to show.
        fake = _fake_tray()
        with _TmpSchedule("night=0\nnext_break=0\nnext_long=0\n"
                          "interval=2\ntotal=4\n"):
            self.assertIsNone(tray.TrayIcon.get_next_break_time(fake))

    def test_night_shows_wake_time(self):
        fake = _fake_tray()
        wake = datetime.datetime.now().timestamp() + 3600
        with _TmpSchedule(f"night=1\nnext_break={int(wake)}\n"
                          f"next_long={int(wake)}\ninterval=0\ntotal=4\n"):
            (short, long_, is_long) = tray.TrayIcon.get_next_break_time(fake)
        self.assertTrue(is_long)
        self.assertEqual(short, long_)

    def test_stale_falls_back_to_upstream(self):
        # Daemon down + file stale -> upstream path (which needs date_time;
        # None -> no info) instead of frozen daemon values.
        fake = _fake_tray()
        with _TmpSchedule("night=0\nnext_break=100\nnext_long=400\n"
                          "interval=1\ntotal=4\n",
                          age=tray.CB_SCHEDULE_STALE_SECS + 60):
            self.assertIsNone(tray.TrayIcon.get_next_break_time(fake))
            self.assertIsNone(tray.TrayIcon.get_interval_progress(fake))


if __name__ == "__main__":
    import unittest
    unittest.main()
