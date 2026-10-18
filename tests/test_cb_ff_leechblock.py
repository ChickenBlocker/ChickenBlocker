import re
import unittest

import cb_ff_leechblock as ff


class AddRegexTests(unittest.TestCase):
    def test_preserves_common_escapes_and_ordinary_sites(self):
        data = {"numSets": "6", "sites2": "example.com"}
        ff.recompile_from_sites(data, 2)

        ff.cmd_add_regex(data, ["2", r"multclub\d+\.lol"])

        self.assertIn(r"multclub\d+\.lol", data["regexpBlock2"])
        regex = re.compile(data["regexpBlock2"])
        self.assertIsNotNone(regex.search("https://example.com/"))
        self.assertIsNotNone(regex.search("https://multclub22.lol/"))
        self.assertIsNone(regex.search("https://multclubabc.lol/"))

    def test_add_sites_rebuilds_combined_regex(self):
        data = {"numSets": "6", "sites2": "example.com"}
        ff.recompile_from_sites(data, 2)
        ff.cmd_add_regex(data, ["2", r"multclub\d+\.lol"])

        ff.cmd_add_sites(data, ["2", "second.example"])

        regex = re.compile(data["regexpBlock2"])
        self.assertIsNotNone(regex.search("https://example.com/"))
        self.assertIsNotNone(regex.search("https://second.example/"))
        self.assertIsNotNone(regex.search("https://multclub22.lol/"))

    def test_appends_to_existing_custom_regex(self):
        data = {
            "numSets": "6",
            "sites2": "example.com",
            "regexpBlock2": r"old\w+",
        }
        ff.recompile_from_sites(data, 2)

        ff.cmd_add_regex(data, ["2", r"new\d+"])

        regex = re.compile(data["regexpBlock2"])
        self.assertIsNotNone(regex.search("https://example.com/"))
        self.assertIsNotNone(regex.search("oldword"))
        self.assertIsNotNone(regex.search("new123"))


if __name__ == "__main__":
    unittest.main()
