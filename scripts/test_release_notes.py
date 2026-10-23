"""Release summaries must match their version and link the relevant commits."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release_notes", Path(__file__).with_name("generate-release-notes.py"))
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)


class ReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.run_git("init", "--quiet")
        self.path = self.root / "installer/releases/v0.1.1.md"
        self.path.parent.mkdir(parents=True)
        self.path.write_text("\n\n".join(notes.HEADINGS) + "\n\n" + notes.MARKER + "\n", encoding="utf-8")
        self.run_git("add", ".")
        self.run_git("commit", "--quiet", "-m", "initial release")
        self.run_git("tag", "v0.1.0")
        (self.root / "feature.txt").write_text("update fixture")
        self.run_git("add", ".")
        self.run_git("commit", "--quiet", "-m", "add *updates* [fixture]")
        self.run_git("tag", "v0.1.1")

    def run_git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Release Fixture",
                               "-c", "user.email=fixture@example.invalid", *args], check=True, capture_output=True)

    def test_version_commit_range(self):
        rendered = notes.render("v0.1.1", self.root)
        self.assertIn(r"add \*updates\* \[fixture\]", rendered)
        self.assertNotIn("initial release", rendered)
        self.assertIn("/compare/v0.1.0...v0.1.1", rendered)
        self.assertNotIn(notes.MARKER, rendered)

    def test_unsafe_or_missing_version(self):
        for tag in ("../v0.1.1", "v0.1.1\n", "v0.1.2", "v01.1.1"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                notes.render(tag, self.root)

    def test_required_sections(self):
        self.path.write_text("Old notes\n" + notes.MARKER)
        with self.assertRaises(ValueError):
            notes.render("v0.1.1", self.root)

    def test_duplicate_marker(self):
        self.path.write_text(self.path.read_text() + notes.MARKER)
        with self.assertRaises(ValueError):
            notes.render("v0.1.1", self.root)


if __name__ == "__main__":
    unittest.main()
