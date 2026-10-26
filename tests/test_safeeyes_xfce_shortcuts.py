import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "vendor"))

from safeeyes.ui import break_screen  # noqa: E402


class XfceShortcutGuardTests(TestCase):
    def test_disable_saves_then_blanks_only_selected_bindings(self):
        calls = []

        def run(*args):
            calls.append(args)
            if len(args) == 4:
                return SimpleNamespace(returncode=0, stdout="old-binding\n")
            return SimpleNamespace(returncode=0, stdout="")

        with tempfile.TemporaryDirectory() as directory:
            state_path = str(Path(directory) / "shortcuts.json")
            with patch.object(
                break_screen, "_shortcut_state_path", return_value=state_path
            ), patch.object(break_screen, "_run_xfconf", side_effect=run):
                self.assertTrue(break_screen._disable_xfce_break_shortcuts())
            self.assertTrue(Path(state_path).exists())

        self.assertEqual(len(calls), len(break_screen._XFCE_BREAK_SHORTCUTS) * 2)
        for channel, prop in break_screen._XFCE_BREAK_SHORTCUTS:
            self.assertIn(("-c", channel, "-p", prop), calls)
            self.assertIn(
                (
                    "-c",
                    channel,
                    "-p",
                    prop,
                    "-s",
                    "/usr/bin/true" if prop.startswith("/commands/") else "",
                ),
                calls,
            )

    def test_command_shortcuts_use_successful_noop(self):
        self.assertEqual(
            break_screen._disabled_shortcut_value("/commands/custom/Super_L"),
            "/usr/bin/true",
        )
        self.assertEqual(
            break_screen._disabled_shortcut_value("/xfwm4/custom/<Alt>Tab"), ""
        )

    def test_restore_rejects_unknown_properties(self):
        saved = [
            {
                "channel": "xfce4-keyboard-shortcuts",
                "property": "/commands/custom/NotOurs",
                "existed": True,
                "value": "command",
            }
        ]
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "shortcuts.json"
            state_path.write_text(break_screen.json.dumps(saved), encoding="utf-8")
            with patch.object(
                break_screen, "_shortcut_state_path", return_value=str(state_path)
            ), patch.object(break_screen, "_run_xfconf") as run:
                break_screen._restore_xfce_break_shortcuts()
            run.assert_not_called()
            self.assertTrue(state_path.exists())

    def test_restore_preserves_exact_values_and_removes_created_properties(self):
        saved = []
        for index, (channel, prop) in enumerate(break_screen._XFCE_BREAK_SHORTCUTS):
            saved.append(
                {
                    "channel": channel,
                    "property": prop,
                    "existed": index != 3,
                    "value": "binding-{}".format(index),
                }
            )

        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "shortcuts.json"
            state_path.write_text(break_screen.json.dumps(saved), encoding="utf-8")
            with patch.object(
                break_screen, "_shortcut_state_path", return_value=str(state_path)
            ), patch.object(
                break_screen, "_run_xfconf", return_value=SimpleNamespace(returncode=0)
            ) as run:
                break_screen._restore_xfce_break_shortcuts()
            self.assertFalse(state_path.exists())

        for index, (channel, prop) in enumerate(break_screen._XFCE_BREAK_SHORTCUTS):
            if index != 3:
                self.assertIn(
                    (("-c", channel, "-p", prop, "-s", "binding-{}".format(index))),
                    [call.args for call in run.call_args_list],
                )
            else:
                self.assertIn(
                    (("-c", channel, "-p", prop, "-r")),
                    [call.args for call in run.call_args_list],
                )
