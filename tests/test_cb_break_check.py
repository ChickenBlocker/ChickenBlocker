import json
import tempfile
from pathlib import Path
from unittest import TestCase
from unittest.mock import patch

import cb_break_check


class ActiveBreakClassificationTests(TestCase):
    def _classify(self, session):
        config = {
            "short_breaks": [{"name": "short queued"}],
            "long_breaks": [{"name": "long queued"}],
        }
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "safeeyes.json"
            session_path = Path(directory) / "session.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            session_path.write_text(json.dumps(session), encoding="utf-8")
            with patch.object(cb_break_check, "CONFIG_PATH", str(config_path)), patch.object(
                cb_break_check, "SESSION_PATH", str(session_path)
            ):
                return cb_break_check._classify()

    def test_active_short_overrides_stale_queued_long_name(self):
        self.assertEqual(
            self._classify(
                {"break": "long queued", "active_break_type": "short"}
            ),
            "BREAK_SHORT",
        )

    def test_active_long_overrides_stale_queued_short_name(self):
        self.assertEqual(
            self._classify(
                {"break": "short queued", "active_break_type": "long"}
            ),
            "BREAK_LONG",
        )

    def test_queued_name_remains_legacy_fallback(self):
        self.assertEqual(self._classify({"break": "short queued"}), "BREAK_SHORT")
