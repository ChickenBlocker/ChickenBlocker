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
                          "interval=3\ntotal=4\ncal_configured=1\n"
                          "cal_ok=500\ncal_fail=600\n"):
            sched = tray._cb_read_daemon_schedule()
        self.assertEqual(sched, {"night": 0, "next_break": 100,
                                 "next_long": 400, "interval": 3, "total": 4,
                                 "cal_configured": 1, "cal_ok": 500,
                                 "cal_fail": 600})

    def test_calendar_fields_default_to_zero(self):
        with _TmpSchedule("night=0\n"):
            sched = tray._cb_read_daemon_schedule()
        self.assertEqual(sched["cal_configured"], 0)
        self.assertEqual(sched["cal_ok"], 0)
        self.assertEqual(sched["cal_fail"], 0)

    def test_missing_file_is_none(self):
        tray.CB_SCHEDULE_FILE = "/tmp/definitely-not-here-cb-schedule"
        self.assertIsNone(tray._cb_read_daemon_schedule())

    def test_stale_file_is_none(self):
        with _TmpSchedule("night=0\n", age=tray.CB_SCHEDULE_STALE_SECS + 60):
            self.assertIsNone(tray._cb_read_daemon_schedule())

    def test_garbage_value_is_none(self):
        with _TmpSchedule("night=soon\n"):
            self.assertIsNone(tray._cb_read_daemon_schedule())


class CalendarStatusTests(TestCase):
    def _status(self, content):
        with _TmpSchedule(content):
            return tray._cb_calendar_status()

    def test_unconfigured_is_hidden(self):
        self.assertIsNone(self._status(
            "night=0\ncal_configured=0\ncal_ok=100\ncal_fail=200\n"))

    def test_daemon_down_is_hidden(self):
        tray.CB_SCHEDULE_FILE = "/tmp/definitely-not-here-cb-schedule"
        self.assertIsNone(tray._cb_calendar_status())

    def test_never_synced(self):
        (label, failing) = self._status("night=0\ncal_configured=1\n")
        self.assertTrue(failing)
        self.assertIn("never synced", label)

    def test_recent_ok_shows_time(self):
        ok = datetime.datetime.now().timestamp() - 120
        (label, failing) = self._status(
            f"night=0\ncal_configured=1\ncal_ok={int(ok)}\ncal_fail=0\n")
        self.assertFalse(failing)
        self.assertIn(tray.utility.format_time(
            datetime.datetime.fromtimestamp(ok)), label)

    def test_ok_after_fail_is_healthy(self):
        ok = datetime.datetime.now().timestamp() - 120
        (label, failing) = self._status(
            f"night=0\ncal_configured=1\ncal_ok={int(ok)}\ncal_fail={int(ok) - 3600}\n")
        self.assertFalse(failing)
        self.assertNotIn("failing", label)

    def test_failing_today_shows_time(self):
        fail = datetime.datetime.now().timestamp() - 120
        (label, failing) = self._status(
            f"night=0\ncal_configured=1\ncal_ok=0\ncal_fail={int(fail)}\n")
        self.assertTrue(failing)
        self.assertIn("failing", label)
        self.assertIn(tray.utility.format_time(
            datetime.datetime.fromtimestamp(fail)), label)

    def test_failing_old_shows_date(self):
        # The silent-OAuth-expiry shape: last failure days ago must render
        # as a DATE, not a misleading HH:MM.
        fail = datetime.datetime.now().timestamp() - 5 * 86400
        (label, failing) = self._status(
            f"night=0\ncal_configured=1\ncal_ok=0\ncal_fail={int(fail)}\n")
        self.assertTrue(failing)
        self.assertIn("failing", label)
        self.assertIn(datetime.datetime.fromtimestamp(fail)
                      .strftime("%Y-%m-%d"), label)

    def _fake_tooltip_tray(self, next_break=None):
        return SimpleNamespace(
            plugin_config={"show_time_in_tray": False},
            get_next_break_time=Mock(return_value=next_break),
            sni_service=Mock(),
        )

    def test_tooltip_renamed_and_carries_failure(self):
        fake = self._fake_tooltip_tray()
        fail = datetime.datetime.now().timestamp() - 120
        with _TmpSchedule(
                f"night=0\ncal_configured=1\ncal_ok=0\ncal_fail={int(fail)}\n"):
            tray.TrayIcon.update_tooltip(fake)
        fake.sni_service.set_tooltip.assert_called_once()
        (title, description) = fake.sni_service.set_tooltip.call_args[0]
        self.assertEqual(title, "ChickenBlocker")
        self.assertIn("failing", description)

    def test_tooltip_clean_when_sync_healthy(self):
        fake = self._fake_tooltip_tray()
        ok = datetime.datetime.now().timestamp() - 120
        with _TmpSchedule(
                f"night=0\ncal_configured=1\ncal_ok={int(ok)}\ncal_fail=0\n"):
            tray.TrayIcon.update_tooltip(fake)
        (title, description) = fake.sni_service.set_tooltip.call_args[0]
        self.assertEqual(title, "ChickenBlocker")
        self.assertNotIn("Calendar", description)

    def test_menu_item_hidden_when_unconfigured(self):
        with _TmpSchedule("night=0\ncal_configured=0\n"):
            item = tray._cb_calendar_menu_item()
        self.assertTrue(item["hidden"])

    def test_menu_item_shows_warning_when_failing(self):
        fail = datetime.datetime.now().timestamp() - 120
        with _TmpSchedule(
                f"night=0\ncal_configured=1\ncal_ok=0\ncal_fail={int(fail)}\n"):
            item = tray._cb_calendar_menu_item()
        self.assertFalse(item.get("hidden", False))
        self.assertIn("failing", item["label"])
        self.assertEqual(item["icon-name"], "dialog-warning")

    def test_menu_item_has_no_warning_icon_when_ok(self):
        ok = datetime.datetime.now().timestamp() - 120
        with _TmpSchedule(
                f"night=0\ncal_configured=1\ncal_ok={int(ok)}\ncal_fail=0\n"):
            item = tray._cb_calendar_menu_item()
        self.assertFalse(item.get("hidden", False))
        self.assertNotIn("icon-name", item)


class TrayDisplayTests(TestCase):
    def test_times_use_24_hour_format(self):
        self.assertEqual(
            tray.utility.format_time(datetime.datetime(2026, 1, 1, 17, 5)),
            "17:05",
        )

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

    def test_monitor_event_refreshes_menu_and_tooltip(self):
        # The Gio.FileMonitor "changed" handler must unconditionally refresh
        # so a daemon republish is reflected without waiting for any event
        # from SafeEyes itself.
        fake = SimpleNamespace(update_menu=Mock(), update_tooltip=Mock())
        tray.TrayIcon._on_daemon_schedule_changed(fake, None, None, None, None)
        fake.update_menu.assert_called_once()
        fake.update_tooltip.assert_called_once()


class ManualBreakTests(TestCase):
    def test_after_long_cycle_manual_break_uses_next_short_cycle(self):
        fake = SimpleNamespace(take_break=Mock())
        with _TmpSchedule(
            "night=0\nnext_break=100\nnext_long=400\ninterval=1\ntotal=4\n"
        ):
            tray.TrayIcon.on_manual_break_clicked(fake, None)

        fake.take_break.assert_called_once_with(tray.BreakType.SHORT_BREAK)

    def test_manual_break_is_long_when_fourth_cycle_is_next(self):
        fake = SimpleNamespace(take_break=Mock())
        with _TmpSchedule(
            "night=0\nnext_break=100\nnext_long=100\ninterval=4\ntotal=4\n"
        ):
            tray.TrayIcon.on_manual_break_clicked(fake, None)

        fake.take_break.assert_called_once_with(tray.BreakType.LONG_BREAK)

    def test_explicit_type_is_preserved(self):
        fake = SimpleNamespace(take_break=Mock())
        with _TmpSchedule(
            "night=0\nnext_break=100\nnext_long=400\ninterval=1\ntotal=4\n"
        ):
            tray.TrayIcon.on_manual_break_clicked(fake, tray.BreakType.LONG_BREAK)

        fake.take_break.assert_called_once_with(tray.BreakType.LONG_BREAK)

    def test_missing_daemon_schedule_falls_back_to_safeeyes_queue(self):
        fake = SimpleNamespace(take_break=Mock())
        saved = tray.CB_SCHEDULE_FILE
        tray.CB_SCHEDULE_FILE = "/tmp/definitely-not-here-cb-manual-schedule"
        try:
            tray.TrayIcon.on_manual_break_clicked(fake, None)
        finally:
            tray.CB_SCHEDULE_FILE = saved

        fake.take_break.assert_called_once_with(None)


if __name__ == "__main__":
    import unittest
    unittest.main()
