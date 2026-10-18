import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.core import SafeEyesCore  # noqa: E402
from safeeyes.model import BreakType, State  # noqa: E402
from safeeyes.safeeyes import SafeEyes  # noqa: E402


def _core(state=State.WAITING, **over):
    # Minimum stand-in for a SafeEyesCore instance (mirrors the helper in
    # test_safeeyes_takebreak.py). __wakeup_scheduler and __do_pre_break are
    # name-mangled, so expose them under the mangled names the unbound
    # methods will look up. impose_break/resume_break are PUBLIC methods the
    # SafeEyes app wrapper calls via attribute access -> plain Mocks.
    base = dict(
        context=SimpleNamespace(state=state),
        _break_queue=Mock(),
        _callback=None,
        _timeout_id=None,
        _firing_hook=False,
        _take_break_now=False,
        _imposed=None,
        _taking_break=None,
        running=True,
        impose_break=None,
        resume_break=Mock(),
    )
    base.update(over)
    fake = SimpleNamespace(**base)
    fake._SafeEyesCore__wakeup_scheduler = Mock()
    fake._SafeEyesCore__resume_break = Mock()
    return fake


class CoreImposeBreakTests(TestCase):
    def test_no_wait_armed_returns_false_without_raising(self):
        # A fresh primary has not armed its scheduler wait yet: impose_break
        # must defer (False) so the remote CLI exits non-zero and the daemon
        # retries, never raise (which would kill the primary).
        core = _core(_callback=None, _firing_hook=False)

        result = SafeEyesCore.impose_break(core, BreakType.LONG_BREAK, 1800)

        self.assertFalse(result)
        self.assertIsNone(core._imposed)
        core._SafeEyesCore__wakeup_scheduler.assert_not_called()

    def test_wait_armed_queues_impose_and_returns_true(self):
        wakeup = Mock()
        core = _core(_callback=Mock(), _timeout_id=1)
        core._SafeEyesCore__wakeup_scheduler = wakeup

        result = SafeEyesCore.impose_break(core, BreakType.SHORT_BREAK, 300)

        self.assertTrue(result)
        self.assertEqual(core._imposed, (BreakType.SHORT_BREAK, 300))
        wakeup.assert_called_once_with()

    def test_break_already_up_is_noop_success(self):
        # The daemon polls the overlay and will not double-impose, but if a
        # command races a natural/imposed break, it must be a benign no-op so
        # the daemon stops retrying.
        core = _core(_taking_break=Mock(), _callback=Mock(), _timeout_id=1)

        result = SafeEyesCore.impose_break(core, BreakType.LONG_BREAK, 60)

        self.assertTrue(result)
        self.assertIsNone(core._imposed)
        core._SafeEyesCore__wakeup_scheduler.assert_not_called()

    def test_stopped_core_returns_false(self):
        # The Start-work gate / disable path has no armed wait; the
        # SafeEyes.impose_break wrapper handles it by resuming directly.
        core = _core(running=False)

        result = SafeEyesCore.impose_break(core, BreakType.SHORT_BREAK, 5)

        self.assertFalse(result)
        self.assertIsNone(core._imposed)

    def test_no_break_queue_is_noop_success(self):
        core = _core(_break_queue=None)

        result = SafeEyesCore.impose_break(core, BreakType.SHORT_BREAK, 5)

        self.assertTrue(result)


class DoPreBreakImposedTests(TestCase):
    def _core_for_pre_break(self):
        core = _core(context=SimpleNamespace(state=State.WAITING),
                     postponed=False)
        core.on_pre_break = Mock()
        core.on_start_break = Mock()
        return core

    def test_imposed_request_dispatches_to_resume_break(self):
        # The armed-wait consume path: __do_pre_break must hand the imposed
        # (type, duration) straight to resume_break -- no pre-break warning,
        # no configured duration.
        core = self._core_for_pre_break()
        core._imposed = (BreakType.LONG_BREAK, 1800)

        SafeEyesCore._SafeEyesCore__do_pre_break(core)

        core.resume_break.assert_called_once_with(
            BreakType.LONG_BREAK, 1800
        )
        core.on_pre_break.assert_not_called()
        self.assertIsNone(core._imposed)

    def test_impose_wins_over_take_break_now(self):
        core = self._core_for_pre_break()
        core._imposed = (BreakType.SHORT_BREAK, 42)
        core._take_break_now = True

        SafeEyesCore._SafeEyesCore__do_pre_break(core)

        core.resume_break.assert_called_once_with(
            BreakType.SHORT_BREAK, 42
        )
        self.assertFalse(core._take_break_now)

    def test_stopped_core_clears_pending_impose(self):
        # Regression guard (break -> gate -> break loop): a stop() wakeup
        # that reaches __do_pre_break must drop a pending impose, not show a
        # break behind the Start-work gate.
        core = _core(context=SimpleNamespace(state=State.STOPPED),
                     postponed=False, running=False)
        core.on_pre_break = Mock()
        core.on_start_break = Mock()
        core._SafeEyesCore__resume_break = Mock()
        core._imposed = (BreakType.SHORT_BREAK, 42)
        core._take_break_now = True

        SafeEyesCore._SafeEyesCore__do_pre_break(core)

        self.assertIsNone(core._imposed)
        self.assertFalse(core._take_break_now)
        core._SafeEyesCore__resume_break.assert_not_called()
        core.on_pre_break.assert_not_called()


class AppImposeBreakTests(TestCase):
    def _app(self, core, active=True):
        return SimpleNamespace(
            active=active,
            safe_eyes_core=core,
            plugins_manager=SimpleNamespace(start=Mock()),
            config=Mock(),
            context={"session": {}},
            _persist_imposed_break_name=lambda: None,
        )

    def test_stopped_core_imposes_directly(self):
        # The Start-work-gate / post-relaunch case: the wrapper must mark the
        # core running and show the break via resume_break -- there is no
        # armed scheduler wait to consume.
        core = _core(running=False, _taking_break=None)
        app = self._app(core)

        result = SafeEyes.impose_break(app, BreakType.LONG_BREAK, 900)

        self.assertTrue(result)
        self.assertTrue(core.running)
        core.resume_break.assert_called_once_with(BreakType.LONG_BREAK, 900)

    def test_stopped_inactive_app_activates_plugins(self):
        core = _core(running=False)
        app = self._app(core, active=False)

        result = SafeEyes.impose_break(app, BreakType.SHORT_BREAK, 300)

        self.assertTrue(result)
        self.assertTrue(app.active)
        app.plugins_manager.start.assert_called_once()

    def test_running_core_delegates(self):
        delegate = Mock(return_value=True)
        core = _core(_callback=Mock(), _timeout_id=1, impose_break=delegate)
        app = self._app(core)

        result = SafeEyes.impose_break(app, BreakType.SHORT_BREAK, 10)

        self.assertTrue(result)
        delegate.assert_called_once_with(BreakType.SHORT_BREAK, 10)

    def test_delegate_failure_propagates_for_retry(self):
        # core.impose_break False (no armed wait) must surface as False so
        # do_command_line exits non-zero and the daemon retries.
        delegate = Mock(return_value=False)
        core = _core(_callback=None, impose_break=delegate)
        app = self._app(core)

        result = SafeEyes.impose_break(app, BreakType.SHORT_BREAK, 10)

        self.assertFalse(result)

    def test_break_already_up_is_noop_success(self):
        core = _core(_taking_break=Mock(), impose_break=Mock())
        app = self._app(core)

        result = SafeEyes.impose_break(app, BreakType.SHORT_BREAK, 10)

        self.assertTrue(result)
        core.impose_break.assert_not_called()
        core.resume_break.assert_not_called()


class ImposeRequestParsingTests(TestCase):
    def test_none_when_not_requested(self):
        self.assertIsNone(SafeEyes._get_impose_request({}))

    def test_short_with_seconds(self):
        req = SafeEyes._get_impose_request({"impose-short": "300"})
        self.assertEqual(req, (BreakType.SHORT_BREAK, 300))

    def test_long_with_seconds(self):
        req = SafeEyes._get_impose_request({"impose-long": "1800"})
        self.assertEqual(req, (BreakType.LONG_BREAK, 1800))

    def test_both_is_error(self):
        with self.assertRaises(ValueError):
            SafeEyes._get_impose_request({"impose-short": "1", "impose-long": "2"})

    def test_garbage_duration_is_error(self):
        with self.assertRaises(ValueError):
            SafeEyes._get_impose_request({"impose-short": "soon"})

    def test_out_of_range_duration_is_error(self):
        with self.assertRaises(ValueError):
            SafeEyes._get_impose_request({"impose-short": "0"})
        with self.assertRaises(ValueError):
            SafeEyes._get_impose_request({"impose-long": str(8 * 24 * 3600)})


if __name__ == "__main__":
    import unittest
    unittest.main()
