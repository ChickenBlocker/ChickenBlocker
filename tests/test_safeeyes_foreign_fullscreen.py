import sys
import time
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.ui import break_screen  # noqa: E402
from safeeyes.ui.break_screen import BreakScreen  # noqa: E402


OWN_XID = 0x0BEEF00D
FOREIGN_XID = 0x12345678
FULLSCREEN = "_NET_WM_STATE_FULLSCREEN"
FIREFOX_CLASS = b"firefox\x00Navigator\x00"


class FakeRoot:
    """Stand-in for the root window of a python-xlib display."""

    def __init__(self, active_xid=0, error=False):
        self.active_xid = active_xid
        self.error = error

    def get_full_property(self, atom, prop_type):
        if self.error:
            raise RuntimeError("root property read failed")
        if atom == "_NET_ACTIVE_WINDOW":
            return SimpleNamespace(value=(self.active_xid,))
        return None


class FakeWindow:
    """Stand-in for a foreign X window.

    A property set to None means "property missing" (read succeeds, no
    value); error=True means the read raises (destroyed window race).
    """

    def __init__(
        self,
        state_atoms=None,
        wm_class=FIREFOX_CLASS,
        state_error=False,
        class_error=False,
    ):
        self.state_atoms = state_atoms
        self.wm_class = wm_class
        self.state_error = state_error
        self.class_error = class_error

    def get_full_property(self, atom, prop_type):
        if atom == "_NET_WM_STATE":
            if self.state_error:
                raise RuntimeError("window destroyed")
            if self.state_atoms is None:
                return None
            return SimpleNamespace(value=tuple(self.state_atoms))
        if atom == "WM_CLASS":
            if self.class_error:
                raise RuntimeError("window destroyed")
            if self.wm_class is None:
                return None
            return SimpleNamespace(value=self.wm_class)
        return None


class FakeDisplay:
    """Stand-in for a python-xlib Display.

    intern_atom returns the atom name itself, so tests compare plain
    strings/ints just like python-xlib compares interned atom numbers.
    """

    def __init__(self, active_xid=0, window=None, root=None):
        self._root = root if root is not None else FakeRoot(active_xid)
        self._window = window

    def intern_atom(self, name, only_if_exists=False):
        return name

    def screen(self):
        return SimpleNamespace(root=self._root)

    def create_resource_object(self, rtype, xid):
        if self._window is None:
            raise RuntimeError("no such window 0x%x" % xid)
        return self._window


def _check(display, own_xids=None):
    if own_xids is None:
        own_xids = {OWN_XID}
    return BreakScreen._check_foreign_fullscreen_x11(display, own_xids)


class ParseActiveWindowTests(TestCase):
    def test_none_property(self):
        self.assertIsNone(BreakScreen._parse_active_window_xid(None))

    def test_zero_means_no_active_window(self):
        self.assertIsNone(
            BreakScreen._parse_active_window_xid(SimpleNamespace(value=(0,)))
        )

    def test_missing_value(self):
        self.assertIsNone(BreakScreen._parse_active_window_xid(SimpleNamespace()))
        self.assertIsNone(
            BreakScreen._parse_active_window_xid(SimpleNamespace(value=()))
        )

    def test_extracts_first_xid(self):
        self.assertEqual(
            BreakScreen._parse_active_window_xid(SimpleNamespace(value=(123,))),
            123,
        )


class StateFullscreenTests(TestCase):
    def test_none_or_empty_is_not_fullscreen(self):
        self.assertFalse(BreakScreen._state_atoms_contain_fullscreen(None, FULLSCREEN))
        self.assertFalse(
            BreakScreen._state_atoms_contain_fullscreen(
                SimpleNamespace(value=()), FULLSCREEN
            )
        )

    def test_contains_fullscreen_atom(self):
        prop = SimpleNamespace(value=("_NET_WM_STATE_FOCUSED", FULLSCREEN))
        self.assertTrue(BreakScreen._state_atoms_contain_fullscreen(prop, FULLSCREEN))

    def test_without_fullscreen_atom(self):
        prop = SimpleNamespace(value=("_NET_WM_STATE_ABOVE", "_NET_WM_STATE_STICKY"))
        self.assertFalse(BreakScreen._state_atoms_contain_fullscreen(prop, FULLSCREEN))


class LockerClassTests(TestCase):
    def test_none_or_empty_is_not_locker(self):
        self.assertFalse(BreakScreen._wm_class_is_locker(None))
        self.assertFalse(BreakScreen._wm_class_is_locker(SimpleNamespace(value=b"")))

    def test_ordinary_application_is_not_locker(self):
        self.assertFalse(BreakScreen._wm_class_is_locker(SimpleNamespace(value=FIREFOX_CLASS)))

    def test_known_locker_classes_are_exempt(self):
        for locker in ("xfce4-screensaver", "light-locker", "i3lock", "xsecurelock"):
            prop = SimpleNamespace(
                value=("%s\x00%s" % (locker, locker)).encode("ascii")
            )
            self.assertTrue(
                BreakScreen._wm_class_is_locker(prop),
                "%s should be treated as a locker" % locker,
            )

    def test_match_is_case_insensitive(self):
        prop = SimpleNamespace(value=b"xfce4-screensaver\x00Xfce4-screensaver\x00")
        self.assertTrue(BreakScreen._wm_class_is_locker(prop))


class ForeignFullscreenDecisionTests(TestCase):
    def test_own_safeeyes_xid_no_recovery(self):
        display = FakeDisplay(
            OWN_XID,
            FakeWindow(state_atoms=(FULLSCREEN,), wm_class=FIREFOX_CLASS),
        )
        self.assertFalse(_check(display))

    def test_no_active_window_no_recovery(self):
        display = FakeDisplay(0, FakeWindow(state_atoms=(FULLSCREEN,)))
        self.assertFalse(_check(display))

    def test_missing_active_window_property_no_recovery(self):
        display = FakeDisplay(root=FakeRoot())
        display._root.active_xid = None
        self.assertFalse(_check(display))

    def test_ordinary_foreign_window_no_recovery(self):
        display = FakeDisplay(
            FOREIGN_XID,
            FakeWindow(
                state_atoms=("_NET_WM_STATE_FOCUSED", "_NET_WM_STATE_MAXIMIZED_VERT"),
                wm_class=FIREFOX_CLASS,
            ),
        )
        self.assertFalse(_check(display))

    def test_foreign_window_without_wm_state_no_recovery(self):
        display = FakeDisplay(FOREIGN_XID, FakeWindow(state_atoms=None))
        self.assertFalse(_check(display))

    def test_foreign_fullscreen_application_requests_recovery(self):
        display = FakeDisplay(
            FOREIGN_XID,
            FakeWindow(state_atoms=(FULLSCREEN,), wm_class=FIREFOX_CLASS),
        )
        self.assertTrue(_check(display))

    def test_window_without_wm_class_still_recovers(self):
        display = FakeDisplay(
            FOREIGN_XID, FakeWindow(state_atoms=(FULLSCREEN,), wm_class=None)
        )
        self.assertTrue(_check(display))

    def test_known_screen_locker_fullscreen_no_recovery(self):
        for locker in ("xfce4-screensaver", "light-locker", "i3lock", "xsecurelock"):
            display = FakeDisplay(
                FOREIGN_XID,
                FakeWindow(
                    state_atoms=(FULLSCREEN,),
                    wm_class=("%s\x00%s" % (locker, locker)).encode("ascii"),
                ),
            )
            self.assertFalse(_check(display), locker)

    def test_destroyed_foreign_window_is_benign_race(self):
        display = FakeDisplay(FOREIGN_XID, FakeWindow(state_error=True))
        self.assertFalse(_check(display))

        display = FakeDisplay(
            FOREIGN_XID, FakeWindow(state_atoms=(FULLSCREEN,), class_error=True)
        )
        self.assertFalse(_check(display))

    def test_unreadable_active_window_property_is_benign(self):
        display = FakeDisplay(root=FakeRoot(error=True))
        self.assertFalse(_check(display))

    def test_vanished_active_window_is_benign(self):
        display = FakeDisplay(FOREIGN_XID, window=None)
        self.assertFalse(_check(display))


def _break_screen():
    # A real BreakScreen with a Wayland context avoids opening an X display
    # in __init__; the X11 watch methods are then driven with fakes.
    context = SimpleNamespace(is_wayland=True)
    return BreakScreen(
        application=None,
        context=context,
        on_skipped=lambda: None,
        on_postponed=lambda: None,
    )


class ForeignFullscreenWatchTests(TestCase):
    def test_start_schedules_single_low_frequency_timer(self):
        bs = _break_screen()
        bs.x11_display = Mock()
        bs.windows = []
        with patch.object(
            break_screen.GLib, "timeout_add", return_value=42
        ) as timeout_add:
            bs._BreakScreen__start_foreign_fullscreen_watch_x11()
            interval = timeout_add.call_args[0][0]
            self.assertGreaterEqual(interval, 200)
            self.assertLessEqual(interval, 250)
        self.assertEqual(bs._foreign_fs_timer, 42)

        with patch.object(break_screen.GLib, "timeout_add") as timeout_add:
            bs._BreakScreen__start_foreign_fullscreen_watch_x11()
            timeout_add.assert_not_called()
        self.assertEqual(bs._foreign_fs_timer, 42)

    def test_start_without_x11_display_is_noop(self):
        bs = _break_screen()
        with patch.object(break_screen.GLib, "timeout_add") as timeout_add:
            bs._BreakScreen__start_foreign_fullscreen_watch_x11()
            timeout_add.assert_not_called()
        self.assertIsNone(bs._foreign_fs_timer)

    def test_stop_removes_timer(self):
        bs = _break_screen()
        bs._foreign_fs_timer = 42
        with patch.object(break_screen.GLib, "source_remove") as source_remove:
            bs._BreakScreen__stop_foreign_fullscreen_watch_x11()
            source_remove.assert_called_once_with(42)
        self.assertIsNone(bs._foreign_fs_timer)

    def test_poll_after_teardown_disables_itself(self):
        bs = _break_screen()
        bs._foreign_fs_timer = None
        bs.windows = [Mock()]
        self.assertFalse(bs._BreakScreen__poll_foreign_fullscreen_x11())

    def test_poll_without_windows_stops_timer(self):
        bs = _break_screen()
        bs._foreign_fs_timer = 42
        bs.windows = []
        self.assertFalse(bs._BreakScreen__poll_foreign_fullscreen_x11())
        self.assertIsNone(bs._foreign_fs_timer)

    def test_destroy_all_screens_stops_watch_before_destroying(self):
        bs = _break_screen()
        bs._foreign_fs_timer = 42
        events = []
        win = Mock()
        win.destroy.side_effect = lambda: events.append("destroy")
        bs.windows = [win]

        with patch.object(
            break_screen.GLib,
            "source_remove",
            side_effect=lambda tid: events.append("stop"),
        ):
            bs._BreakScreen__destroy_all_screens()

        self.assertEqual(events, ["stop", "destroy"])
        self.assertEqual(bs.windows, [])

    def test_poll_triggers_recovery_for_foreign_fullscreen(self):
        bs = _break_screen()
        bs._foreign_fs_timer = 42
        bs.x11_display = FakeDisplay(
            FOREIGN_XID, FakeWindow(state_atoms=(FULLSCREEN,), wm_class=FIREFOX_CLASS)
        )
        bs._own_xids_x11 = {OWN_XID}
        win = Mock()
        bs.windows = [win]

        self.assertTrue(bs._BreakScreen__poll_foreign_fullscreen_x11())
        win.present.assert_called_once()
        win.fullscreen_on_monitor.assert_called_once_with(win.fullscreen_monitor)

    def test_poll_resets_failure_budget_when_no_foreign_fullscreen(self):
        bs = _break_screen()
        bs._foreign_fs_timer = 42
        bs.x11_display = FakeDisplay(OWN_XID)
        bs._own_xids_x11 = {OWN_XID}
        bs._foreign_fs_failures = 4
        bs.windows = [Mock()]

        self.assertTrue(bs._BreakScreen__poll_foreign_fullscreen_x11())
        self.assertEqual(bs._foreign_fs_failures, 0)


class RecoveryLimitTests(TestCase):
    def _armed(self, windows=None):
        bs = _break_screen()
        bs.windows = windows if windows is not None else [Mock()]
        bs._foreign_fs_failures = 0
        bs._foreign_fs_last_recovery = 0.0
        return bs

    def test_recovery_restores_each_window(self):
        win = Mock()
        bs = self._armed([win])
        bs.x11_display = None  # __window_set_keep_above_x11 no-ops

        bs._BreakScreen__recover_break_screens_x11()

        win.present.assert_called_once()
        win.fullscreen_on_monitor.assert_called_once_with(win.fullscreen_monitor)

    def test_recovery_rate_limited_to_once_per_second(self):
        win = Mock()
        bs = self._armed([win])
        bs._foreign_fs_last_recovery = time.monotonic()

        bs._BreakScreen__recover_break_screens_x11()

        win.present.assert_not_called()

    def test_recovery_stands_down_after_failure_budget(self):
        win = Mock()
        bs = self._armed([win])
        bs._foreign_fs_failures = 5

        bs._BreakScreen__recover_break_screens_x11()

        win.present.assert_not_called()


if __name__ == "__main__":
    import unittest

    unittest.main()
