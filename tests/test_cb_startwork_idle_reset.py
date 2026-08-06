import sys
import time
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "cb_startwork"))

import plugin as cb_startwork  # noqa: E402


class IdleGateResetTests(TestCase):
    """Regression: an idle gate held across a long away/suspend must not
    restore its stale remaining work time at Start (the Oct 7-8 incident:
    gate armed with 95s left, laptop slept ~15.8h, Start -> 5-min break after
    ~1 min of work). Held >= idle_gate_reset_seconds -> fresh full interval."""

    def setUp(self):
        # Keep the real decision log clean and never touch the daemon sentinel.
        self._handlers = cb_startwork._log.handlers[:]
        cb_startwork._log.handlers = []
        self._clear_flag = cb_startwork._clear_flag
        cb_startwork._clear_flag = Mock()
        cb_startwork.core = SimpleNamespace(running=False, start=Mock())
        cb_startwork._dialog = Mock()
        cb_startwork._idle_gate_reset_seconds = 1800

    def tearDown(self):
        cb_startwork._log.handlers = self._handlers
        cb_startwork._clear_flag = self._clear_flag
        cb_startwork.core = None
        cb_startwork._dialog = None
        cb_startwork._idle_gated = False
        cb_startwork._idle_next_break_delay = None
        cb_startwork._idle_gate_fired_time = None

    def _press_start(self):
        cb_startwork._on_start_clicked(None)
        cb_startwork.core.start.assert_called_once()
        return cb_startwork.core.start.call_args[0][0]

    def test_held_past_reset_drops_stale_remainder(self):
        cb_startwork._idle_gated = True
        cb_startwork._idle_next_break_delay = 95
        cb_startwork._idle_gate_fired_time = time.time() - (15.8 * 3600)

        next_break_time = self._press_start()

        self.assertEqual(next_break_time, -1)

    def test_held_exactly_reset_threshold_drops_remainder(self):
        cb_startwork._idle_gated = True
        cb_startwork._idle_next_break_delay = 95
        cb_startwork._idle_gate_fired_time = time.time() - 1800

        self.assertEqual(self._press_start(), -1)

    def test_short_hold_restores_remaining(self):
        cb_startwork._idle_gated = True
        cb_startwork._idle_next_break_delay = 95
        cb_startwork._idle_gate_fired_time = time.time() - 300

        next_break_time = self._press_start()
        expected = time.time() + 95

        self.assertGreaterEqual(next_break_time, expected - 5)
        self.assertLessEqual(next_break_time, expected + 5)

    def test_missing_fire_time_restores_remaining(self):
        # Defensive: an armed gate without a fire timestamp (should not
        # happen) keeps the old restore behavior rather than resetting.
        cb_startwork._idle_gated = True
        cb_startwork._idle_next_break_delay = 95
        cb_startwork._idle_gate_fired_time = None

        next_break_time = self._press_start()
        expected = time.time() + 95

        self.assertGreaterEqual(next_break_time, expected - 5)
        self.assertLessEqual(next_break_time, expected + 5)

    def test_natural_end_gate_still_fresh_interval(self):
        cb_startwork._idle_gated = False
        cb_startwork._idle_next_break_delay = None
        cb_startwork._idle_gate_fired_time = None

        self.assertEqual(self._press_start(), -1)

    def test_state_reset_after_start(self):
        cb_startwork._idle_gated = True
        cb_startwork._idle_next_break_delay = 95
        cb_startwork._idle_gate_fired_time = time.time() - 7200

        self._press_start()

        self.assertFalse(cb_startwork._idle_gated)
        self.assertIsNone(cb_startwork._idle_next_break_delay)
        self.assertIsNone(cb_startwork._idle_gate_fired_time)
        self.assertIsNone(cb_startwork._dialog)

    def test_custom_threshold_respected(self):
        cb_startwork._idle_gate_reset_seconds = 300
        cb_startwork._idle_gated = True
        cb_startwork._idle_next_break_delay = 95
        cb_startwork._idle_gate_fired_time = time.time() - 301

        self.assertEqual(self._press_start(), -1)
