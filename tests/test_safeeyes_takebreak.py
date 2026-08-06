import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.core import SafeEyesCore  # noqa: E402
from safeeyes.model import State  # noqa: E402
from safeeyes import utility  # noqa: E402


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


class DoPreBreakStoppedCoreTests(TestCase):
    def test_stopped_core_never_starts_break_despite_take_break_now(self):
        # Regression (suspend/resume break loop): core.stop() (the
        # cb_startwork Start-work gate or the suspend handler) wakes an armed
        # wait and runs __do_pre_break. If an external -t had left
        # _take_break_now set, the old check order started a break on the
        # ALREADY STOPPED core -- behind the Start-work gate. The running
        # check must come first and must clear the stale request.
        core = _core(
            context=SimpleNamespace(state=State.STOPPED, postponed=False),
            running=False,
            _take_break_now=True,
        )
        core.on_pre_break = Mock()
        core.on_start_break = Mock()

        SafeEyesCore._SafeEyesCore__do_pre_break(core)

        self.assertFalse(core._take_break_now)
        core.on_pre_break.assert_not_called()
        core.on_start_break.assert_not_called()


class ExecuteMainThreadTests(TestCase):
    def test_truthy_target_return_does_not_repeat_idle_callback(self):
        calls = []

        def target():
            calls.append(True)
            return True

        def run_idle(callback):
            # Model GLib: a truthy callback return schedules another invocation.
            for _ in range(3):
                if not callback():
                    break

        with patch.object(utility.GLib, "idle_add", side_effect=run_idle):
            utility.execute_main_thread(target)

        self.assertEqual(calls, [True])


if __name__ == "__main__":
    import unittest

    unittest.main()
