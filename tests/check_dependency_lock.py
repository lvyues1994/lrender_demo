"""Exercise the CMake verifier using disposable repositories, never dependencies."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

VERIFIER = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path("cmake/VerifyDependencies.cmake").resolve()

class DependencyLockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / "dep"
        self.repo.mkdir()
        self.git("init", "-q")
        (self.repo / "source.txt").write_text("source\n")
        self.git("add", "source.txt")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@invalid.example", "commit", "-qm", "fixture")
        self.sha = self.git("rev-parse", "HEAD").strip()
        self.lock = self.root / "lock.json"
        self.record = {"path": "dep", "url": "https://example.invalid/dep.git", "branch": "fixture", "sha": self.sha, "upstreamPin": self.sha}

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], text=True)

    def verify(self):
        self.lock.write_text(json.dumps({"records": [self.record]}))
        return subprocess.run(["cmake", "-DDEMO_DEPENDENCY_ROOT=" + str(self.root), "-DDEMO_DEPENDENCY_LOCK=" + str(self.lock), "-P", str(VERIFIER)], capture_output=True, text=True)

    def rejected(self, reason):
        result = self.verify()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(reason, result.stdout + result.stderr)

    def test_valid_locked_repository(self):
        result = self.verify()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Dependency lock verified: 1", result.stdout)

    def test_wrong_sha_rejected(self):
        self.record["sha"] = "0" * 40
        self.rejected("SHA mismatch")

    def test_missing_directory_rejected(self):
        self.record["path"] = "missing"
        self.rejected("Missing dependency directory")

    def test_dirty_repository_rejected(self):
        (self.repo / "source.txt").write_text("changed\n")
        self.rejected("Dirty dependency")

    def test_non_repository_rejected(self):
        (self.root / "not-git").mkdir()
        self.record["path"] = "not-git"
        self.rejected("Not a Git checkout")

    def test_path_traversal_rejected(self):
        self.record["path"] = "../outside"
        self.rejected("Unsafe dependency path")

    def test_escaping_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as outside:
            (self.root / "escape").symlink_to(outside, target_is_directory=True)
            self.record["path"] = "escape"
            self.rejected("Unsafe dependency path")

    def configure(self):
        self.lock.write_text(json.dumps({"records": [self.record]}))
        source = self.root / "project"
        source.mkdir(exist_ok=True)
        guard = VERIFIER.with_name("DependencyGuard.cmake")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(guard NONE)\n'
            f'set(DEMO_DEPENDENCY_ROOT "{self.root}")\n'
            f'set(DEMO_DEPENDENCY_LOCK "{self.lock}")\n'
            f'include("{VERIFIER}")\ninclude("{guard}")\n')
        return subprocess.run(["cmake", "-S", str(source), "-B", str(self.root / "build")], capture_output=True, text=True)

    def test_configure_wrong_sha(self):
        self.record["sha"] = "0" * 40
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA mismatch", result.stdout + result.stderr)

    def test_configure_missing_dependency(self):
        self.record["path"] = "missing"
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing dependency directory", result.stdout + result.stderr)

    def test_cached_build_rechecks_dirty_dependency(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (self.repo / "source.txt").write_text("changed after configure\n")
        result = subprocess.run(["cmake", "--build", str(self.root / "build")], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Dirty dependency", result.stdout + result.stderr)

if __name__ == "__main__":
    unittest.main(verbosity=2)
