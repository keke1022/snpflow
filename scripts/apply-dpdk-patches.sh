#!/usr/bin/env bash
# Apply the complete validated series to a clean DPDK baseline checkout.
set -euo pipefail

if [[ $# != 1 ]]; then
    echo "Usage: $0 /path/to/clean/dpdk-25.11.3" >&2
    exit 2
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
series_dir="$repo_root/patches/dpdk-25.11.3"
dpdk_dir=$(cd -- "$1" && pwd)
source_tree=$(git -C "$dpdk_dir" rev-parse 'HEAD^{tree}')
if [[ "$source_tree" == $(cat "$series_dir/RELEASE_BASE_TREE") ]]; then
    expected_result=$(cat "$series_dir/RELEASE_RESULT_TREE")
elif [[ "$source_tree" == $(cat "$series_dir/BASE_TREE") ]]; then
    expected_result=$(cat "$series_dir/RESULT_TREE")
else
    echo "Source tree does not match a recorded DPDK 25.11.3 baseline." >&2
    echo "See patches/README.md; no changes were made." >&2
    exit 1
fi

if [[ -n $(git -C "$dpdk_dir" status --porcelain) ]]; then
    echo "DPDK checkout must be clean, including untracked files." >&2
    exit 1
fi

patches=()
while IFS= read -r name; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    patches+=("$series_dir/$name")
done < "$series_dir/series"

if ! git -C "$dpdk_dir" am "${patches[@]}"; then
    git -C "$dpdk_dir" am --abort
    echo "Patch application failed; the series was rolled back." >&2
    exit 1
fi

actual_tree=$(git -C "$dpdk_dir" rev-parse 'HEAD^{tree}')
if [[ "$actual_tree" != "$expected_result" ]]; then
    echo "Patches applied, but the result differs from the validated tree." >&2
    exit 1
fi
echo "Applied ${#patches[@]} patches; validated DPDK tree: $actual_tree"
