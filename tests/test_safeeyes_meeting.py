from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.safeeyes import SafeEyes  # noqa: E402


class SafeEyesMeetingPauseTests(unittest.TestCase):
    """VENDORED CHANGE (ChickenBlocker): with daemon-owned break timing there
    is NO work-countdown preservation across a meeting pause anymore. The
    daemon disarms its own work deadline (se_work_end) while the pause holds
    and arms a FRESH interval when it ends, so the plugin-side pause/resume
    pair is now a plain disable/enable of the (neutered) scheduler."""

    def test_pause_is_plain_disable(self):
        disable = Mock()
        fake = SimpleNamespace(
            active=True,
            disable_safeeyes=disable,
        )

        SafeEyes.pause_for_meeting(fake)

        disable.assert_called_once_with()

    def test_pause_when_inactive_is_noop(self):
        disable = Mock()
        fake = SimpleNamespace(
            active=False,
            disable_safeeyes=disable,
        )

        SafeEyes.pause_for_meeting(fake)

        disable.assert_not_called()

    def test_suspend_resume_is_plain_enable_without_preserved_time(self):
        # Regression: the old path scheduled enable_safeeyes(now + delay),
        # which armed a natural break `delay` seconds later. With the
        # neutered scheduler that must never happen: the resume is a plain
        # enable with no scheduled time at all.
        enable = Mock()
        fake = SimpleNamespace(
            enable_safeeyes=enable,
        )

        SafeEyes.resume_meeting_after_suspend(fake)

        enable.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
