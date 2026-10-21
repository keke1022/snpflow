#!/usr/bin/env python3
"""Check publishable contents, local Markdown links, and reachable blob sizes."""

from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def main():
    errors = []
    names = set(git("ls-files", "-z", "--cached", "--others", "--exclude-standard").decode().split("\0"))
    for name in sorted(names - {""}):
        path = ROOT / name
        if not path.is_file():  # A tracked deletion in the current worktree.
            continue
        if path.stat().st_size > 10 * 1024 * 1024:
            errors.append(f"Oversized repository file: {name}")
        with path.open("rb") as stream:
            if stream.read(4) == b"\x7fELF":
                errors.append(f"Compiled ELF artifact would be published: {name}")
        if path.suffix != ".md":
            continue
        for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", path.read_text()):
            link = urlsplit(target.strip("<>"))
            if link.scheme or link.netloc or not link.path:
                continue
            if not (path.parent / unquote(link.path)).exists():
                errors.append(f"Broken local link in {name}: {target}")

    series_dir = ROOT / "patches/dpdk-25.11.3"
    series = [line for line in (series_dir / "series").read_text().splitlines()
              if line and not line.startswith("#")]
    if len(series) != len(set(series)):
        errors.append("Duplicate patch in series")
    if set(series) != {p.name for p in series_dir.glob("*.patch")}:
        errors.append("Patch files and series manifest differ")
    for name in series:
        if not (series_dir / name).is_file():
            errors.append(f"Missing patch: {name}")

    # Verify the standalone protocol core matches the code shipped in DPDK.
    patch = (series_dir / "0008-testpmd-cvm-authenticated-chunks.patch").read_text()
    for section in patch.split("diff --git ")[1:]:
        if "new file mode" not in section:
            continue
        header = re.search(r"^\+\+\+ b/app/test-pmd/(cvm_chunk_\S+)", section, re.M)
        if not header:
            continue
        source = ROOT / "scratch/cvm-chunk-proto" / header.group(1)
        added = "".join(line[1:] + "\n" for line in section.splitlines()
                        if line.startswith("+") and not line.startswith("+++"))
        if not source.is_file() or source.read_text() != added:
            errors.append(f"Protocol source differs from DPDK patch: {source.name}")

    objects = git("rev-list", "--objects", "--all")
    sizes = subprocess.check_output(
        ["git", "-C", str(ROOT), "cat-file", "--batch-check=%(objecttype) %(objectsize) %(rest)"],
        input=objects,
    ).decode()
    for line in sizes.splitlines():
        kind, size, *name = line.split(" ", 2)
        if kind == "blob" and int(size) >= 100 * 1024 * 1024:
            errors.append(f"Reachable Git blob exceeds 100 MiB: {' '.join(name)} ({size} bytes)")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Repository checks passed: local links, patch manifest, protocol sources, artifacts, Git blob sizes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
