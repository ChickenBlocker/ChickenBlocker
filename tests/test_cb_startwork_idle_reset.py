import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
# Vendor FIRST: plugin.py lazily does `from safeeyes.model import State` at
# import time; without this the apt copy at /usr/lib/python3/dist-packages
# gets cached in sys.modules and poisons every later test that imports the
# vendored package (unittest discover imports this module first).
sys.path.insert(0, str(ROOT / "vendor"))
sys.path.insert(0, str(ROOT / "cb_startwork"))

import plugin as cb_startwork  # noqa: E402


class StartFreshTests(TestCase):
    """VENDORED CHANGE (ChickenBlocker): the daemon owns work-interval timing.
    Pressing Start must ALWAYS restart the (neutered) scheduler fresh --
    core.start() with no preserved next-break time -- so SafeEyes can never
    fire a natural break from plugin-restored state. The freeze/hold-reset
    rules for the work deadline now live in the ChickenBlocker daemon, which
    arms its own se_work_end when the sentinel flag clears."""

    def setUp(self):
        # Keep the real decision log clean and never touch the daemon sentinel.
        self._handlers = cb_startwork._log.handlers[:]
        cb_startwork._log.handlers = []
        self._clear_flag = cb_startwork._clear_flag
        cb_startwork._clear_flag = Mock()
        self._set_flag = cb_startwork._set_flag
        self._request_daemon_refresh = cb_startwork._request_daemon_refresh
        cb_startwork._request_daemon_refresh = Mock()
        cb_startwork.core = SimpleNamespace(running=False, start=Mock(), stop=Mock())
        cb_startwork._dialog = Mock()

    def tearDown(self):
        cb_startwork._log.handlers = self._handlers
        cb_startwork._clear_flag = self._clear_flag
        cb_startwork._set_flag = self._set_flag
        cb_startwork._request_daemon_refresh = self._request_daemon_refresh
        cb_startwork.core = None
        cb_startwork._dialog = None
        cb_startwork._idle_gated = False

    def _press_start(self):
        cb_startwork._on_start_clicked(None)
        cb_startwork.core.start.assert_called_once()
        return cb_startwork.core.start.call_args

    def test_start_always_fresh_no_preserved_time(self):
        # Even with an idle gate that froze mid-work, Start passes NO
        # next-break time: restoring one would schedule a natural break,
        # which the neutered scheduler must never fire.
        cb_startwork._idle_gated = True

        call_args = self._press_start()

        self.assertEqual(call_args[0], ())
        self.assertEqual(call_args[1], {})

    def test_start_after_break_end_is_fresh(self):
        cb_startwork._idle_gated = False

        call_args = self._press_start()

        self.assertEqual(call_args[0], ())

    def test_start_clears_flag_and_state(self):
        cb_startwork._idle_gated = True

        cb_startwork._on_start_clicked(None)

        cb_startwork._clear_flag.assert_called_once()
        cb_startwork._request_daemon_refresh.assert_called_once()
        self.assertFalse(cb_startwork._idle_gated)
        self.assertIsNone(cb_startwork._dialog)

    def test_start_destroys_dialog_even_when_core_running(self):
        # Defense-in-depth path: core already running (stale scheduler leak).
        # Start must still clear the gate state and destroy the dialog.
        cb_startwork.core = SimpleNamespace(running=True, start=Mock())
        cb_startwork._idle_gated = True

        cb_startwork._on_start_clicked(None)

        cb_startwork.core.start.assert_not_called()
        cb_startwork._clear_flag.assert_called_once()
        self.assertFalse(cb_startwork._idle_gated)
        self.assertIsNone(cb_startwork._dialog)

    def test_idle_gate_sets_sentinel_and_stops_core(self):
        # The idle gate itself still freezes the (neutered) scheduler and
        # shows the dialog; the DAEMON freezes its own work deadline when it
        # sees the sentinel on its next cycle.
        cb_startwork._dialog = None  # no dialog up yet
        show_dialog = Mock()
        set_flag = Mock()
        cb_startwork._show_dialog = show_dialog
        cb_startwork._set_flag = set_flag
        try:
            cb_startwork._do_idle_gate()
        finally:
            cb_startwork._show_dialog = None
            cb_startwork._set_flag = self._set_flag

        self.assertTrue(cb_startwork._idle_gated)
        set_flag.assert_called_once()
        cb_startwork.core.stop.assert_called_once()
