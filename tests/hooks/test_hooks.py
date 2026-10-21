"""Exercise hooks against real disposable Git repositories and staged blobs."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/git-hooks.py"


class HooksTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="snpflow-hook-test-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.startswith("GIT_") or key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
                self.env.pop(key)
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.git("init", "-q")
        self.git("config", "user.name", "keke1022")
        self.git("config", "user.email", "keke83space@gmail.com")

    def run_command(self, command, *, data=None, env=None):
        return subprocess.run(command, cwd=self.repo, env=env or self.env,
                              input=data, text=True, capture_output=True)

    def git(self, *args):
        result = self.run_command(["git", *args])
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def write(self, name, content, stage=True):
        target = self.repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        if stage:
            self.git("add", "-f", "--", name)

    def hook(self, command="check-index", *args, data=None, env=None):
        return self.run_command([sys.executable, str(SCRIPT), command, *args], data=data, env=env)

    def assert_pass(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_blocked(self, result, reason):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(reason, result.stdout + result.stderr)

    def install(self):
        shutil.copytree(ROOT / ".githooks", self.repo / ".githooks")
        (self.repo / "scripts").mkdir()
        shutil.copy2(SCRIPT, self.repo / "scripts/git-hooks.py")
        self.assert_pass(self.hook("install"))

    def test_initial_commit_and_unusual_names(self):
        self.write("space tab\t newline\n中文.txt", b"hello\n")
        self.assert_pass(self.hook())

    def test_checks_staged_bad_content_even_if_worktree_fixed(self):
        self.write("test.txt", b"bad \n")
        self.write("test.txt", b"fixed\n", stage=False)
        self.assert_blocked(self.hook(), "trailing whitespace")

    def test_ignores_unstaged_bad_content(self):
        self.write("test.txt", b"good\n")
        self.write("test.txt", b"bad \n", stage=False)
        self.assert_pass(self.hook())

    def test_elf_without_extension(self):
        self.write("program", b"\x7fELF\x00")
        self.assert_blocked(self.hook(), "Compiled ELF")

    def test_large_blob(self):
        self.write("big", b"x" * (10 * 1024 * 1024 + 1))
        self.assert_blocked(self.hook(), "exceeds 10 MiB")

    def test_private_key_does_not_print_contents(self):
        for kind in (b"", b"RSA ", b"EC ", b"OPENSSH ", b"ENCRYPTED "):
            self.write("key.txt", b"-----BEGIN " + kind + b"PRIVATE KEY-----\nsecret-value\n")
            result = self.hook()
            self.assert_blocked(result, "Private key material")
            self.assertNotIn("secret-value", result.stdout + result.stderr)

    def test_conflict_markers(self):
        self.write("source.c", b"<" * 7 + b" HEAD\nleft\n" + b"=" * 7 + b"\nright\n")
        self.assert_blocked(self.hook(), "Merge conflict marker")

    def test_artifact_paths(self):
        for name in ("capture.pcap", "module.ko", "library.so.1", ".env", ".env.local"):
            with self.subTest(name=name):
                self.write(name, b"example\n")
                self.assertNotEqual(self.hook().returncode, 0)
                self.git("rm", "--cached", "--", name)

    def test_env_example_and_patch_whitespace_allowed(self):
        self.write(".env.example", b"TOKEN=\n")
        self.write(".gitattributes", b"*.patch -whitespace\n")
        self.write("change.patch", b" context\n \n")
        self.assert_pass(self.hook())

    def test_symlink_is_not_followed(self):
        outside = Path(self.temp.name) / "outside"
        outside.write_bytes(b"\x7fELF")
        (self.repo / "link").symlink_to(outside)
        self.git("add", "link")
        self.assert_pass(self.hook())

    def test_unstaged_attributes_cannot_disable_checks(self):
        self.write(".gitattributes", b"*.txt whitespace\n")
        self.write(".gitattributes", b"*.txt -whitespace\n", stage=False)
        self.write("bad.txt", b"bad \n")
        self.assert_blocked(self.hook(), "trailing whitespace")

    def test_staged_patch_exemption_survives_unstaged_attributes(self):
        self.write(".gitattributes", b"*.patch -whitespace\n")
        self.write(".gitattributes", b"*.patch whitespace\n", stage=False)
        self.write("change.patch", b" context\n \n")
        self.assert_pass(self.hook())

    def test_unresolved_merge(self):
        self.write("conflict", b"base\n")
        self.git("commit", "-qm", "test: base")
        self.git("checkout", "-qb", "side")
        self.write("conflict", b"side\n")
        self.git("commit", "-qm", "test: side")
        self.git("checkout", "-qb", "other", "HEAD~1")
        self.write("conflict", b"other\n")
        self.git("commit", "-qm", "test: other")
        self.assertNotEqual(self.run_command(["git", "merge", "side"]).returncode, 0)
        self.assert_blocked(self.hook(), "Unresolved merge")

    def test_deleted_file(self):
        self.write("test.txt", b"good\n")
        self.git("commit", "-qm", "test: initial")
        self.git("rm", "test.txt")
        self.assert_pass(self.hook())

    def test_all_checks_unchanged_committed_blobs(self):
        self.write("program", b"\x7fELF")
        self.git("commit", "-qm", "test: bad fixture")
        self.assert_pass(self.hook())
        self.assert_blocked(self.hook("check-index", "--all"), "Compiled ELF")

    def test_install_uninstall_and_actual_commit_hook(self):
        self.install()
        self.assert_pass(self.hook("install"))
        self.assertEqual(self.git("config", "--local", "--get", "core.hooksPath"), ".githooks")
        self.write("bad.txt", b"bad \n")
        self.assert_blocked(self.run_command(["git", "commit", "-m", "test: rejected"]),
                            "trailing whitespace")
        self.assert_pass(self.hook("uninstall"))
        self.assert_pass(self.hook("uninstall"))
        self.assertNotEqual(self.run_command(["git", "config", "--get", "core.hooksPath"]).returncode, 0)

    def test_preserve_existing_hook_configuration(self):
        self.git("config", "core.hooksPath", "custom-hooks")
        self.assert_blocked(self.hook("install"), "leaving it unchanged")
        self.assert_pass(self.hook("uninstall"))
        self.assertEqual(self.git("config", "core.hooksPath"), "custom-hooks")

    def test_preserve_existing_default_hooks(self):
        hook = self.repo / ".git/hooks/pre-commit"
        hook.write_text("#!/bin/sh\nexit 0\n")
        hook.chmod(0o755)
        self.assert_blocked(self.hook("install"), "Existing hooks")

    def push_input(self, revision):
        return f"refs/heads/main {revision} refs/heads/main {'0' * 40}\n"

    def test_push_checks_requested_commit_and_keeps_dirty_worktree(self):
        self.write("Makefile", b"test:\n\t@test \"$$(cat marker)\" = committed\ncheck:\n\t@true\n")
        self.write("marker", b"committed\n")
        self.git("commit", "-qm", "test: passing snapshot")
        revision = self.git("rev-parse", "HEAD")
        self.write("marker", b"new-head\n")
        self.git("commit", "-qm", "test: different head")
        self.write("marker", b"staged\n")
        self.write("marker", b"unstaged\n", stage=False)
        self.write("untracked", b"keep\n", stage=False)
        before = self.git("status", "--porcelain=v1")
        # Simulate Git's exported context, including an alternate index path.
        env = dict(self.env, GIT_DIR=str(self.repo / ".git"),
                   GIT_WORK_TREE=str(self.repo), GIT_INDEX_FILE=str(self.repo / ".git/index"))
        self.assert_pass(self.hook("pre-push", data=self.push_input(revision), env=env))
        self.assertEqual(self.git("status", "--porcelain=v1"), before)
        self.assertEqual(self.git("show", ":marker"), "staged")
        self.assertEqual((self.repo / "marker").read_text(), "unstaged\n")
        self.assertEqual((self.repo / "untracked").read_text(), "keep\n")

    def test_push_blocks_failed_tests(self):
        self.write("Makefile", b"test:\n\t@false\ncheck:\n\t@true\n")
        self.git("commit", "-qm", "test: failing snapshot")
        self.assert_blocked(self.hook("pre-push", data=self.push_input(self.git("rev-parse", "HEAD"))),
                            "SNPFlow hook failed")

    def test_push_delete_and_empty_input(self):
        self.assert_pass(self.hook("pre-push", data=""))
        self.assert_pass(self.hook("pre-push", data=self.push_input("0" * 40)))

    def test_push_annotated_tag_and_duplicate_tip(self):
        self.write("Makefile", b"test check:\n\t@true\n")
        self.git("commit", "-qm", "test: snapshot")
        revision = self.git("rev-parse", "HEAD")
        self.git("tag", "-am", "test tag", "v-test")
        tag = self.git("rev-parse", "v-test")
        result = self.hook("pre-push", data=self.push_input(revision) + self.push_input(tag))
        self.assert_pass(result)
        self.assertEqual(result.stdout.count("Testing push commit"), 1)

    def test_real_git_push_rejects_failure(self):
        self.install()
        self.write("Makefile", b"test:\n\t@false\ncheck:\n\t@true\n")
        self.git("commit", "-qm", "test: failing snapshot")
        remote = Path(self.temp.name) / "remote.git"
        self.git("init", "--bare", "-q", str(remote))
        self.assert_blocked(self.run_command(["git", "push", str(remote), "HEAD:refs/heads/main"]),
                            "SNPFlow hook failed")
        self.assertNotEqual(self.run_command(["git", "--git-dir", str(remote),
                                             "rev-parse", "--verify", "refs/heads/main"]).returncode, 0)

    def test_real_git_push_accepts_passing_commit(self):
        self.install()
        self.write("Makefile", b"test check:\n\t@true\n")
        self.git("commit", "-qm", "test: passing snapshot")
        revision = self.git("rev-parse", "HEAD")
        remote = Path(self.temp.name) / "remote.git"
        self.git("init", "--bare", "-q", str(remote))
        self.assert_pass(self.run_command(["git", "push", str(remote), "HEAD:refs/heads/main"]))
        self.assertEqual(self.git("--git-dir", str(remote), "rev-parse", "refs/heads/main"), revision)


if __name__ == "__main__":
    unittest.main()
