#!/usr/bin/env python3
"""Optional Git hooks. Never rewrite the index or the developer's worktree."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


MAX_FILE_SIZE = 10 * 1024 * 1024
PRIVATE_KEY = re.compile(rb"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----")
CONFLICT = re.compile(rb"^(?:<{7}(?: |$)|={7}$|>{7}(?: |$))", re.M)
ARTIFACT_SUFFIXES = {".o", ".a", ".so", ".ko", ".pyc", ".pyo", ".pcap", ".pcapng"}


def git(*args, cwd=None):
    return subprocess.check_output(["git", *args], cwd=cwd)


def check_index(all_files=False):
    # Read object IDs from the index, not file contents from the worktree.
    changed = None if all_files else set(git(
        "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"
    ).split(b"\0"))
    errors = []
    for entry in git("ls-files", "--stage", "-z").split(b"\0"):
        if not entry:
            continue
        metadata, raw_name = entry.split(b"\t", 1)
        mode, oid, stage = metadata.split()
        name = os.fsdecode(raw_name)
        if stage != b"0":
            errors.append(f"Unresolved merge: {name!r}")
            continue
        if changed is not None and raw_name not in changed:
            continue
        if mode == b"160000":  # A submodule points to a commit, not a blob.
            continue
        path = Path(name)
        if path.suffix.lower() in ARTIFACT_SUFFIXES or ".so." in path.name:
            errors.append(f"Build output or packet capture: {name!r}")
        if path.name == ".env" or (path.name.startswith(".env.")
                                   and path.name != ".env.example"):
            errors.append(f"Local environment file: {name!r}")
        size = int(git("cat-file", "-s", oid.decode()))
        if size > MAX_FILE_SIZE:
            errors.append(f"File exceeds 10 MiB: {name!r}")
            continue
        data = git("cat-file", "blob", oid.decode())
        if data.startswith(b"\x7fELF"):
            errors.append(f"Compiled ELF binary: {name!r}")
        if PRIVATE_KEY.search(data):
            errors.append(f"Private key material: {name!r}")
        if CONFLICT.search(data):
            errors.append(f"Merge conflict marker: {name!r}")
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    # Use staged attributes too: an unstaged .gitattributes edit must not
    # suppress errors or break the repository's intentional patch exemption.
    tree = git("write-tree").decode().strip()
    whitespace = subprocess.run(["git", f"--attr-source={tree}", "diff",
                                 "--cached", "--check", "--no-ext-diff", "--no-textconv"])
    if whitespace.returncode:
        return 1
    print("Index checks passed.")
    return 0


def install(uninstall=False):
    root = Path(os.fsdecode(git("rev-parse", "--show-toplevel")).strip())
    current = subprocess.run(["git", "config", "--get", "core.hooksPath"],
                             stdout=subprocess.PIPE, text=True)
    if current.returncode not in (0, 1):
        return current.returncode
    configured = current.stdout.strip() if current.returncode == 0 else None
    if uninstall:
        local = subprocess.run(["git", "config", "--local", "--get", "core.hooksPath"],
                               stdout=subprocess.PIPE, text=True)
        if local.returncode != 0 or local.stdout.strip() != ".githooks":
            print("No SNPFlow local hook configuration to remove.")
            return 0
        subprocess.run(["git", "config", "--local", "--unset-all", "core.hooksPath"],
                       check=True)
        print("Removed SNPFlow local hooks; inherited Git configuration applies again.")
        return 0
    if configured is not None and configured != ".githooks":
        print(f"Existing core.hooksPath={configured!r}; leaving it unchanged. "
              "Integrate the SNPFlow checks into your existing hooks manually.", file=sys.stderr)
        return 1
    # Also preserve default hooks that setting hooksPath would hide.
    default_hooks = Path(os.fsdecode(git("rev-parse", "--git-path", "hooks")).strip())
    active = [p.name for p in default_hooks.glob("*")
              if p.is_file() and os.access(p, os.X_OK) and not p.name.endswith(".sample")]
    if configured is None and active:
        print(f"Existing hooks {active!r}; leaving them active. Integrate manually.",
              file=sys.stderr)
        return 1
    for name in ("pre-commit", "pre-push"):
        if not os.access(root / ".githooks" / name, os.X_OK):
            raise RuntimeError(f"Missing executable hook: {name}")
    subprocess.run(["git", "config", "--local", "core.hooksPath", ".githooks"], check=True)
    print("Installed SNPFlow hooks for this repository (core.hooksPath=.githooks).")
    return 0


def pre_push():
    revisions = []
    for line in sys.stdin:
        local_ref, local_oid, _remote_ref, _remote_oid = line.split()
        if set(local_oid) == {"0"}:  # Deleting a remote ref requires no build.
            continue
        result = subprocess.run(["git", "rev-parse", "--verify", f"{local_oid}^{{commit}}"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            raise RuntimeError(f"Cannot test {local_ref}: it does not resolve to a commit")
        revision = result.stdout.strip()
        if revision not in revisions:
            revisions.append(revision)
    if not revisions:
        return 0
    source = os.fsdecode(git("rev-parse", "--show-toplevel")).strip()
    # Git exports repository-specific variables in hook contexts. Clear them
    # before working in the disposable clone, including alternate index paths.
    env = os.environ.copy()
    for key in git("rev-parse", "--local-env-vars").decode().splitlines():
        env.pop(key, None)
    # Do not inherit make's jobserver, -n, or user build-directory overrides.
    for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "GIT_CONFIG_COUNT"):
        env.pop(key, None)
    checker = Path(__file__).resolve()
    for revision in revisions:
        print(f"Testing push commit {revision[:12]} in an isolated checkout...", flush=True)
        with tempfile.TemporaryDirectory(prefix="snpflow-push-") as directory:
            checkout = Path(directory) / "repo"
            subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "clone", "--quiet",
                            "--shared", "--no-checkout", "--", source, str(checkout)],
                           env=env, check=True)
            subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "checkout", "--quiet",
                            "--detach", revision], cwd=checkout, env=env, check=True)
            # Use this hook's checker even when pushing a pre-hook revision.
            subprocess.run([sys.executable, str(checker), "check-index", "--all"],
                           cwd=checkout, env=env, check=True)
            subprocess.run(["make", "test", "check", f"PYTHON={sys.executable}"],
                           cwd=checkout, env=env, check=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check-index", "install", "uninstall", "pre-push"))
    parser.add_argument("--all", action="store_true", help="check every indexed file")
    args = parser.parse_args()
    try:
        if args.command == "check-index":
            return check_index(args.all)
        if args.command in ("install", "uninstall"):
            return install(args.command == "uninstall")
        return pre_push()
    except (subprocess.CalledProcessError, OSError, RuntimeError, ValueError) as exc:
        print(f"SNPFlow hook failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
