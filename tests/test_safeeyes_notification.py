import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.model import State  # noqa: E402
from safeeyes.plugins.notification import plugin  # noqa: E402


class DaemonScheduleNotificationTests(TestCase):
    def setUp(self):
        plugin.context = SimpleNamespace(state=State.WAITING)
        plugin.warning_time = 10
        plugin.notified_deadline = None

    def test_warns_once_when_short_break_is_ten_seconds_away(self):
        schedule = {"night": "0", "next_break": "1010", "interval": "2"}
        with patch.object(plugin, "_read_daemon_schedule", return_value=schedule), \
                patch.object(plugin.time, "time", return_value=1000), \
                patch.object(plugin, "_show_notification") as show:
            self.assertTrue(plugin._check_daemon_schedule())
            self.assertTrue(plugin._check_daemon_schedule())

        show.assert_called_once_with(False, 10)

    def test_does_not_warn_early(self):
        schedule = {"night": "0", "next_break": "1011", "interval": "1"}
        with patch.object(plugin, "_read_daemon_schedule", return_value=schedule), \
                patch.object(plugin.time, "time", return_value=1000), \
                patch.object(plugin, "_show_notification") as show:
            plugin._check_daemon_schedule()

        show.assert_not_called()

    def test_fourth_interval_is_labeled_long(self):
        schedule = {"night": "0", "next_break": "1005", "interval": "4"}
        with patch.object(plugin, "_read_daemon_schedule", return_value=schedule), \
                patch.object(plugin.time, "time", return_value=1000), \
                patch.object(plugin, "_show_notification") as show:
            plugin._check_daemon_schedule()

        show.assert_called_once_with(True, 5)

    def test_ignores_schedule_outside_waiting_state(self):
        plugin.context.state = State.BREAK
        with patch.object(plugin, "_read_daemon_schedule", Mock()) as read_schedule:
            plugin._check_daemon_schedule()

        read_schedule.assert_not_called()


if __name__ == "__main__":
    import unittest

    unittest.main()
