#!/usr/bin/env bash
# Copy sources to an explicitly selected CVM. Preserve remote build outputs.
set -euo pipefail
if [[ $# != 1 || "$1" != *:* ]]; then
    echo "Usage: $0 user@host:/path/to/snp-dpdk/" >&2
    exit 2
fi
rsync -az --exclude '.git' --exclude '.agents' --exclude '.codex' \
    --exclude 'build' --exclude '.venv' --exclude '__pycache__' \
    --exclude '.env*' --exclude '*.pcap' --exclude '*.pcapng' \
    --exclude '*.ko' --exclude '*.o' --exclude '*.mod*' --exclude '.*.cmd' \
    --exclude 'Module.symvers' --exclude 'modules.order' \
    --exclude '/snp-probe' --exclude '/devx-probe' --exclude '/shared-probe' \
    --exclude '/verify-probe' --exclude '/nop-probe' --exclude '/bounds-test' \
    --exclude '/devx-size' --exclude '/dmabuf-variants' \
    --exclude 'kv_send' --exclude 'copy_bench' --exclude 'cuda_probe' \
    --exclude 'probe_user' --exclude 'cvm_chunk_test' --exclude 'cvm_chunk_send' \
    "$(dirname "$(readlink -f "$0")")/" "$1"
echo "Sources copied to $1"
