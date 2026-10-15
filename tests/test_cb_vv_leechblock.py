import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "cb_vv_leechblock", ROOT / "cb_vv_leechblock.py"
)
vv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vv)

try:
    import plyvel
    HAVE_PLYVEL = True
except ImportError:
    HAVE_PLYVEL = False


def as_dict(items):
    return dict(items)


class ManagedItemsPkfTests(unittest.TestCase):
    def test_injects_pkf_words_into_set5(self):
        pkf = {"keywords": ["foo", "bar"], "titleOnly": True}
        items = as_dict(vv.managed_items({"numSets": "6"}, pkf))

        self.assertEqual(items["sites5"], "* ~foo ~bar")
        # keywordRE5 is the compiled ~token regex (same compiler as sites),
        # and the "*" gate keeps blockRE5 matching every page.
        compiled = vv.sites_to_regexps("* ~foo ~bar", False)
        self.assertEqual(items["keywordRE5"], compiled["keyword"])
        self.assertTrue(compiled["keyword"])
        self.assertEqual(items["blockRE5"], compiled["block"])
        self.assertTrue(items["blockRE5"].startswith("^(https?|file)"))
        # PKF-mirrored behaviour flags.
        self.assertIs(items["titleOnly5"], True)
        self.assertIs(items["allowKeywords5"], False)
        self.assertIs(items["closeTab5"], True)
        self.assertIs(items["showTimer5"], False)
        # Active 24/7, no limit, not disabled.
        self.assertIs(items["activeBlock5"], True)
        self.assertIs(items["disable5"], False)
        self.assertEqual(items["days5"], [True] * 7)
        self.assertEqual(items["times5"], "0000-2359")
        self.assertEqual(items["limitMins5"], "")
        self.assertGreaterEqual(items["numSets"], 5)

    def test_pkf_words_override_even_nonempty_firefox_set5(self):
        # Firefox blanks set 5, but if it ever holds stale data the PKF list
        # still wins for the injected set.
        ff = {"numSets": "6", "sites5": "stale.example",
              "keywordRE5": "stale"}
        items = as_dict(vv.managed_items(ff, {"keywords": ["foo"]}))
        self.assertEqual(items["sites5"], "* ~foo")
        self.assertEqual(
            items["keywordRE5"], vv.sites_to_regexps("* ~foo", False)["keyword"])

    def test_no_pkf_words_leaves_set5_mirror_empty(self):
        for pkf in (None, {}, {"keywords": [], "titleOnly": True}):
            items = as_dict(vv.managed_items({"numSets": "6"}, pkf))
            self.assertEqual(items["sites5"], "")
            self.assertEqual(items["keywordRE5"], "")
            self.assertIs(items["activeBlock5"], False)
            self.assertIs(items["closeTab5"], False)

    def test_pkf_words_are_sanitised(self):
        pkf = {"keywords": ["foo", "", 7, None, "bar"], "titleOnly": False}
        items = as_dict(vv.managed_items({}, pkf))
        self.assertEqual(items["sites5"], "* ~foo ~bar")


class ManagedItemsRegexpBlockTests(unittest.TestCase):
    def test_regexpblock_is_mirrored(self):
        ff = {"numSets": "6", "regexpBlock6": "foo|bar", "sites6": ""}
        items = as_dict(vv.managed_items(ff, None))
        self.assertEqual(items["regexpBlock6"], "foo|bar")
        # URL-regex-only set still counts as non-empty -> forced active.
        self.assertIs(items["activeBlock6"], True)

    def test_regexpblock_empty_by_default(self):
        items = as_dict(vv.managed_items({}, None))
        for n in range(1, vv.NUM_SETS + 1):
            self.assertEqual(items["regexpBlock%d" % n], "")


class PkfPathTests(unittest.TestCase):
    def test_snap_layout(self):
        sqlite = ("/home/u/snap/firefox/common/.mozilla/firefox/"
                  "abc.default/storage-sync-v2.sqlite")
        self.assertEqual(
            vv.pkf_manifest_path(sqlite),
            "/home/u/snap/firefox/common/.mozilla/managed-storage/"
            "page-keyword-filter-7f3a@local.addons.json")

    def test_classic_layout(self):
        sqlite = "/home/u/.mozilla/firefox/abc.default/storage-sync-v2.sqlite"
        self.assertEqual(
            vv.pkf_manifest_path(sqlite),
            "/home/u/.mozilla/managed-storage/"
            "page-keyword-filter-7f3a@local.addons.json")


class LoadPkfTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cbvv_pkf_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _sqlite_path(self):
        prof = os.path.join(self.tmp, ".mozilla", "firefox", "p.default")
        os.makedirs(prof, exist_ok=True)
        return os.path.join(prof, "storage-sync-v2.sqlite")

    def _write_manifest(self, obj):
        mandir = os.path.join(self.tmp, ".mozilla", "managed-storage")
        os.makedirs(mandir, exist_ok=True)
        with open(vv.pkf_manifest_path(self._sqlite_path()), "w",
                  encoding="utf-8") as f:
            json.dump(obj, f)

    def test_loads_words_and_title_only(self):
        self._write_manifest({"type": "storage", "data": {
            "keywords": ["foo", "bar"], "titleOnly": True}})
        self.assertEqual(
            vv.load_pkf(self._sqlite_path()),
            {"keywords": ["foo", "bar"], "titleOnly": True})

    def test_missing_manifest_returns_none(self):
        self.assertIsNone(vv.load_pkf(self._sqlite_path()))

    def test_malformed_or_wrong_type_returns_none(self):
        self._write_manifest({"type": "extension", "data": {"keywords": ["x"]}})
        self.assertIsNone(vv.load_pkf(self._sqlite_path()))
        path = vv.pkf_manifest_path(self._sqlite_path())
        with open(path, "w", encoding="utf-8") as f:
            f.write("{not json")
        self.assertIsNone(vv.load_pkf(self._sqlite_path()))


class OwnershipGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cbvv_own_")
        self.parent = os.path.join(self.tmp, "Local Extension Settings")
        self.db = os.path.join(self.parent, "blaaajhemilngeeffpbfkdjjoefldkok")
        os.makedirs(self.db)
        for f in ("CURRENT", "LOCK", "000003.log"):
            with open(os.path.join(self.db, f), "wb") as fh:
                fh.write(b"x")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_mismatch_false_for_uniform_owner(self):
        self.assertFalse(vv.ownership_mismatch(self.db))

    def test_mismatch_true_when_file_owner_differs(self):
        real_stat = os.stat
        db_file = os.path.join(self.db, "LOCK")

        def fake_stat(p, *a, **k):
            st = real_stat(p)
            if p == db_file:
                import types
                st = types.SimpleNamespace(
                    **{k: getattr(st, k) for k in
                       ("st_uid", "st_gid", "st_mode", "st_size", "st_mtime")})
                st.st_uid = st.st_uid + 1
            return st

        with unittest.mock.patch("os.stat", side_effect=fake_stat):
            self.assertTrue(vv.ownership_mismatch(self.db))

    def test_mismatch_true_when_unreadable(self):
        with unittest.mock.patch("os.walk", side_effect=OSError):
            self.assertTrue(vv.ownership_mismatch(self.db))

    def test_chown_noop_for_unprivileged(self):
        calls = []
        with unittest.mock.patch("os.geteuid", return_value=1000), \
                unittest.mock.patch("os.lchown", side_effect=calls.append):
            vv.chown_to_owner(self.db)
        self.assertEqual(calls, [])


@unittest.skipUnless(HAVE_PLYVEL, "plyvel not available")
class LevelDbRoundTripTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cbvv_db_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _drift(self, db, items):
        # Same comparison --diff performs on the live snapshot.
        return sum(1 for k, v in items
                   if db.get(k.encode()) != vv.compact(v))

    def test_sync_then_diff_reports_ok_and_detects_new_words(self):
        ff = {"numSets": "6", "regexpBlock6": "foo|bar"}
        pkf = {"keywords": ["foo"], "titleOnly": True}
        db = plyvel.DB(os.path.join(self.tmp, "db"), create_if_missing=True)
        items = vv.managed_items(ff, pkf)
        for k, v in items:
            db.put(k.encode(), vv.compact(v))
        # Idempotent write: nothing changes, diff is clean.
        self.assertEqual(self._drift(db, vv.managed_items(ff, pkf)), 0)
        # A newly added PKF word produces drift on the recompiled set-5 keys
        # (sites5 + keywordRE5; blockRE5 is the "*" gate and stays identical)
        # - the daemon then kill+sync+relaunches Vivaldi via the existing
        # cooldown logic.
        drift = self._drift(db, vv.managed_items(
            ff, {"keywords": ["foo", "baz"], "titleOnly": True}))
        self.assertEqual(drift, 2)
        # A regexpBlock edit on Firefox set 6 also shows up as drift.
        self.assertEqual(self._drift(db, vv.managed_items(
            {"numSets": "6", "regexpBlock6": "foo|bar|qux"}, pkf)), 1)
        db.close()


if __name__ == "__main__":
    unittest.main()
