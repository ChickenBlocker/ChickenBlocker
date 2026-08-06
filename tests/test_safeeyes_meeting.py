import datetime
from pathlib import Path
import sys
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.model import State  # noqa: E402
from safeeyes.safeeyes import SafeEyes  # noqa: E402


class SafeEyesMeetingPauseTests(unittest.TestCase):
    def test_pause_records_waiting_work_countdown(self):
        disable = Mock()
        fake = SimpleNamespace(
            active=True,
            safe_eyes_core=SimpleNamespace(
                context=SimpleNamespace(state=State.WAITING),
                scheduled_next_break_time=datetime.datetime.now()
                + datetime.timedelta(seconds=300),
            ),
            _meeting_next_break_delay=None,
            _meeting_pause_monotonic=None,
            disable_safeeyes=disable,
        )

        SafeEyes.pause_for_meeting(fake)

        self.assertGreaterEqual(fake._meeting_next_break_delay, 299)
        self.assertLessEqual(fake._meeting_next_break_delay, 300)
        self.assertIsNotNone(fake._meeting_pause_monotonic)
        disable.assert_called_once_with()

    def test_suspend_resume_charges_only_awake_confirmation_time(self):
        enable = Mock()
        fake = SimpleNamespace(
            _meeting_next_break_delay=300,
            _meeting_pause_monotonic=time.monotonic() - 30,
            enable_safeeyes=enable,
        )

        before = datetime.datetime.now().timestamp()
        SafeEyes.resume_meeting_after_suspend(fake)
        after = datetime.datetime.now().timestamp()

        scheduled = enable.call_args.args[0]
        self.assertGreaterEqual(scheduled, before + 269)
        self.assertLessEqual(scheduled, after + 270)

    def test_break_pause_resumes_as_fresh_work_interval(self):
        enable = Mock()
        fake = SimpleNamespace(
            _meeting_next_break_delay=None,
            _meeting_pause_monotonic=time.monotonic(),
            enable_safeeyes=enable,
        )

        SafeEyes.resume_meeting_after_suspend(fake)

        enable.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
