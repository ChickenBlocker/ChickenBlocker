import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.core import SafeEyesCore  # noqa: E402
from safeeyes.model import State  # noqa: E402


def _core(state=State.WAITING, **over):
    # Minimum stand-in for a SafeEyesCore instance: take_break only touches
    # these attributes. `state` is placed on context.state (what take_break
    # actually checks). __wakeup_scheduler is name-mangled, so expose it under
    # the mangled name the unbound method will look up.
    base = dict(
        context=SimpleNamespace(state=state),
        _break_queue=Mock(),
        _callback=None,
        _timeout_id=None,
        _firing_hook=False,
        _take_break_now=False,
    )
    base.update(over)
    fake = SimpleNamespace(**base)
    fake._SafeEyesCore__wakeup_scheduler = Mock()
    return fake


class TakeBreakNotArmedTests(TestCase):
    def test_no_wait_armed_returns_false_without_raising(self):
        # Regression: an external `safeeyes -t/-b` arriving while the scheduler
        # has no __wait_for armed (state==WAITING, _callback==None) used to make
        # __wakeup_scheduler raise "trying to queue action while core is not
        # running", killing the primary. It must now return False (defer) and
        # leave _take_break_now untouched.
        core = _core(_callback=None, _firing_hook=False)

        result = SafeEyesCore.take_break(core)

        self.assertFalse(result)
        self.assertFalse(core._take_break_now)
        core._SafeEyesCore__wakeup_scheduler.assert_not_called()

    def test_wait_armed_queues_break_and_returns_true(self):
        wakeup = Mock()
        core = _core(_callback=Mock(), _timeout_id=1)
        core._SafeEyesCore__wakeup_scheduler = wakeup

        result = SafeEyesCore.take_break(core)

        self.assertTrue(result)
        self.assertTrue(core._take_break_now)
        wakeup.assert_called_once_with()

    def test_not_waiting_is_noop_success(self):
        # A break requested while already in a break (or otherwise not WAITING)
        # is a benign no-op: nothing to take, report success so the caller stops
        # retrying.
        core = _core(state=State.BREAK, _callback=Mock(), _timeout_id=1)

        result = SafeEyesCore.take_break(core)

        self.assertTrue(result)
        self.assertFalse(core._take_break_now)
        core._SafeEyesCore__wakeup_scheduler.assert_not_called()

    def test_no_break_queue_is_noop_success(self):
        core = _core(_break_queue=None)

        result = SafeEyesCore.take_break(core)

        self.assertTrue(result)


if __name__ == "__main__":
    import unittest

    unittest.main()
